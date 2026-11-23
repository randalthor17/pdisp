package dev.pdisp

import android.app.Activity
import android.app.KeyguardManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.content.IntentFilter
import android.view.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** Where the user wants to be connected. It outlives any one session: while [SessionController.wantConnected] is set the app keeps (re)connecting. */
sealed class Target {
    data class Host(val pc: Pc) : Target()
    object Usb : Target()
}

enum class Phase { Idle, Connecting, Pairing, Streaming }

/** Live numbers while streaming. */
data class Stats(val width: Int, val height: Int, val fps: Float, val lostFrames: Int, val idrRequests: Int)

/** Something went wrong: what happened, in words for a person, and what to try. */
data class Problem(val title: String, val hint: String?)

/**
 * The connection to the PC as the screens see it: what phase it is in, the pairing code, live numbers and the last problem.
 * Owns the native session, reconnects after a lost link, and handles the USB accessory.
 */
class SessionController(private val activity: Activity, private val store: Store) {
    var phase by mutableStateOf(Phase.Idle); private set
    var target by mutableStateOf<Target?>(null); private set
    var stage by mutableStateOf(0); private set
    var pairCode by mutableStateOf<String?>(null); private set
    var stats by mutableStateOf<Stats?>(null); private set
    /** The screen brightness the PC asked for (0..100, for instance from a DDC/CI slider), or -1 if it never did. */
    var pcBrightness by mutableStateOf(-1); private set
    var problem by mutableStateOf<Problem?>(null)
    var attempts by mutableStateOf(0); private set
    /** True when a connecting stage has lasted more than a few seconds: the screen then says what usually causes it. */
    var slow by mutableStateOf(false); private set
    private var stageSinceMs = 0L
    private var lastStage = -1
    /** The text the native side reports (diagnostics, and the fallback when no stage text fits). */
    var rawStatus by mutableStateOf(""); private set
    /** Set while the app waits for a USB cable or for the user's permission. */
    var usbHint by mutableStateOf<String?>(null); private set

    private var handle = 0L
    var wantConnected = false; private set
    private var surface: Surface? = null
    private var pendingAccessory: UsbAccessory? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private val ui = Handler(Looper.getMainLooper())
    private val usb = activity.getSystemService(Context.USB_SERVICE) as UsbManager
    private val reconnect = Runnable { connectNow() }
    /** Which way the connection was going when it was lost: after a loss the other way is tried too (see [connectNow]). */
    private var primaryUsb = false

    val handleOrZero: Long get() = handle
    val active: Boolean get() = handle != 0L

    // ---- the surface

    fun surfaceCreated(s: Surface) {
        surface = s
        pendingAccessory?.let { connectUsb(it) } ?: connectNow() // coming back to the app (after the screen was off): pick up where we were
    }

    /** Leaving the screen (the screen turned off, a lock, another app): end the stream, saying why, but remember that we want it back. */
    fun surfaceDestroyed() {
        surface = null
        ui.removeCallbacks(reconnect)
        stopSession(closeReason())
    }

    // ---- what the user asks for

    fun connect(pc: Pc) {
        problem = null
        if (handle != 0L && (target as? Target.Host)?.pc?.host == pc.host) return
        if (handle != 0L) stopSession(0) // an address chosen on purpose wins over a USB session that nobody answers
        ui.removeCallbacks(reconnect)
        wantConnected = true
        target = Target.Host(pc)
        attempts = 0
        phase = Phase.Connecting
        startHostSession(pc.host)
    }

    fun connectUsb(acc: UsbAccessory) {
        problem = null
        wantConnected = true
        if (handle != 0L) {
            if (target !is Target.Host) return // a session over the cable is already running
            // Streaming over Wi-Fi and the cable has just come in (the PC asked this device to switch to it): the cable is the better way. Leave Wi-Fi the way
            // a lost link is left (the PC keeps the monitor for a while and gives it back to this device), then connect over USB.
            stopSession(3)
            attempts = 0
        }
        target = Target.Usb
        phase = Phase.Connecting
        if (!usb.hasPermission(acc)) {
            val pi = PendingIntent.getBroadcast(activity, 0, Intent(ACTION_USB_PERMISSION).setPackage(activity.packageName), PendingIntent.FLAG_MUTABLE)
            usb.requestPermission(acc, pi)
            usbHint = "Waiting for you to allow USB access"
            return
        }
        if (surface == null) { pendingAccessory = acc; usbHint = "USB connected, preparing the display"; return }
        pendingAccessory = null
        val pfd = usb.openAccessory(acc)
        if (pfd == null) { fail(Problem("Cannot open the USB connection", "Unplug the cable and plug it in again.")); return }
        val d = displayInfo()
        usbHint = null
        handle = PdispNative.nativeStartUsb(
            pfd.detachFd(), surface!!, d.width, d.height, activity.resources.displayMetrics.densityDpi, d.rates, Build.MODEL,
            activity.filesDir.absolutePath, deviceCaps(), Codecs.mask(store.settings),
        )
        sessionStarted()
    }

    fun usbPermissionResult(acc: UsbAccessory?, granted: Boolean) {
        if (acc != null && granted) connectUsb(acc)
        else fail(Problem("USB access was not allowed", "Plug the cable in again and choose Allow when Android asks."))
    }

    /** The user ends the connection: no reconnecting, and the PC removes the monitor at once. */
    fun disconnect() {
        wantConnected = false
        pendingAccessory = null
        usbHint = null
        ui.removeCallbacks(reconnect)
        stopSession(0)
        phase = Phase.Idle
        target = null
    }

    fun displayChanged() {
        if (handle != 0L) {
            val d = displayInfo()
            PdispNative.nativeDisplayChanged(handle, d.width, d.height)
        }
    }

    fun settingsChanged() {
        if (handle != 0L) PdispNative.nativeSetPalmRejection(handle, store.settings.palmRejection, palmSizePx())
    }

    // ---- connecting

    private fun connectNow() {
        if (!wantConnected || handle != 0L || surface == null) return
        val t = target ?: return
        val acc = usb.accessoryList?.firstOrNull()
        // The PC to try over the network: the one asked for, or, when the way was the cable, the one used last.
        val net = (t as? Target.Host)?.pc ?: store.pcs().filter { it.lastUsed > 0 }.maxByOrNull { it.lastUsed }
        // After a lost link both ways are tried: the other one first (the one that just failed is the likelier to be down), two tries each, so an
        // unplugged cable moves a session to Wi-Fi at once, a dropped Wi-Fi moves it to the cable, and a dead one is not insisted on.
        val useUsb = when {
            acc != null && net != null ->
                if (attempts == 0) t is Target.Usb else if (((attempts - 1) / 2) % 2 == 0) !primaryUsb else primaryUsb
            acc != null -> true
            net != null -> false
            else -> { usbHint = usbWaitingText(); scheduleReconnect(); return }
        }
        if (useUsb) connectUsb(acc!!)
        else { target = Target.Host(net!!); usbHint = null; startHostSession(net.host) }
    }

    private fun startHostSession(address: String) {
        val s = surface ?: return
        val host = address.substringBefore(':') // "host" or "host:port" (a PC found on the network may use another port)
        val port = address.substringAfter(':', "").toIntOrNull() ?: 47110
        val d = displayInfo()
        phase = Phase.Connecting
        handle = PdispNative.nativeStart(
            host, port, s, d.width, d.height, activity.resources.displayMetrics.densityDpi, d.rates, Build.MODEL,
            activity.filesDir.absolutePath, deviceCaps(), Codecs.mask(store.settings),
        )
        sessionStarted()
    }

    private fun sessionStarted() {
        if (handle == 0L) return
        PdispNative.nativeSetPalmRejection(handle, store.settings.palmRejection, palmSizePx())
        // match the panel's refresh to the stream: 60 fps on a 90 Hz panel lands on uneven vsync slots
        activity.window.attributes = activity.window.attributes.also { it.preferredRefreshRate = 60f }
        // Wi-Fi power save batches packets for tens of milliseconds, which shows up as stutter
        val wm = activity.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "pdisp").also { it.acquire() }
        ui.post(poll)
    }

    private val poll = object : Runnable {
        override fun run() {
            if (handle == 0L) return
            val text = PdispNative.nativeStatus(handle)
            rawStatus = text
            if (PdispNative.nativeFinished(handle)) { sessionEnded(text); return }
            val info = PdispNative.nativeInfo(handle)
            stage = info[0]
            if (info.size > 10 && info[10] != pcBrightness) pcBrightness = info[10]
            if (stage != lastStage) { lastStage = stage; stageSinceMs = System.currentTimeMillis() }
            slow = stage != STAGE_STREAMING && stage != STAGE_PAIRING && System.currentTimeMillis() - stageSinceMs > 4000
            when (stage) {
                STAGE_PAIRING -> { pairCode = "%06d".format(info[1]); phase = Phase.Pairing }
                STAGE_STREAMING -> {
                    if (phase != Phase.Streaming) {
                        (target as? Target.Host)?.let { store.used(it.pc.id) }
                        attempts = 0
                    }
                    pairCode = null
                    phase = Phase.Streaming
                    stats = Stats(info[2], info[3], info[4] / 10f, info[6], info[7])
                }
                else -> { pairCode = null; if (phase != Phase.Streaming) phase = Phase.Connecting }
            }
            ui.postDelayed(this, 500)
        }
    }

    /** The native session ended by itself: tidy up, and try again if the link failed and the user still wants the connection. */
    private fun sessionEnded(message: String) {
        val kind = PdispNative.nativeEndKind(handle)
        stopSession(3)
        // "busy" while reconnecting is the PC not having noticed yet that the old link is dead (up to a few seconds): ask again
        val busyAgain = kind == 2 && attempts in 1..7 && message.contains("busy")
        if (wantConnected && (kind == 1 || busyAgain)) {
            if (attempts == 0) primaryUsb = target is Target.Usb // the way that just failed
            attempts++
            phase = Phase.Connecting
            rawStatus = "lost the PC, reconnecting: $message"
            stage = STAGE_RECONNECTING
            scheduleReconnect()
        } else { // ended on purpose, refused or impossible: back to the start, saying why
            wantConnected = false
            problem = explain(message)
            phase = Phase.Idle
            target = null
        }
    }

    private fun fail(p: Problem) {
        wantConnected = false
        problem = p
        phase = Phase.Idle
        target = null
    }

    private fun scheduleReconnect() {
        ui.removeCallbacks(reconnect)
        ui.postDelayed(reconnect, minOf(1000L * (attempts + 1), 5000L))
    }

    private fun stopSession(reason: Int) {
        val h = handle
        handle = 0
        ui.removeCallbacks(poll)
        if (h != 0L) PdispNative.nativeStop(h, reason)
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        stats = null
        pairCode = null
        slow = false
        lastStage = -1
        if (phase == Phase.Streaming || phase == Phase.Pairing) phase = if (wantConnected) Phase.Connecting else Phase.Idle
    }

    /** Why we are leaving, as the PC should hear it: the screen locking or turning off is not the same as the user ending the stream. */
    private fun closeReason(): Int {
        val pm = activity.getSystemService(Context.POWER_SERVICE) as PowerManager
        val km = activity.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        return if (!pm.isInteractive || km.isKeyguardLocked) 1 else 0
    }

    /** Android keeps a new USB connection charging-only until the user allows data; then the PC's request to switch to accessory mode is ignored. */
    private fun usbWaitingText(): String {
        val plugged = activity.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        return if (plugged and BatteryManager.BATTERY_PLUGGED_USB != 0)
            "Cable connected, but USB is charging only: open the USB notification and choose File transfer"
        else "Waiting for the USB cable"
    }

    // ---- the display

    class DisplayInfo(val width: Int, val height: Int, val rates: IntArray)

    /** The display's real pixel size (landscape, rounded down to a multiple of 8 as the encoder needs) and its refresh rates in mHz, within the user's cap. */
    fun displayInfo(): DisplayInfo {
        val b = activity.windowManager.currentWindowMetrics.bounds // follows the current rotation (this activity is full screen)
        val w = b.width() and 7.inv()
        val h = b.height() and 7.inv()
        val display = if (Build.VERSION.SDK_INT >= 30) activity.display!! else @Suppress("DEPRECATION") activity.windowManager.defaultDisplay
        val size = display.mode
        val cap = store.settings.maxRefreshHz
        val all = display.supportedModes
            .filter { maxOf(it.physicalWidth, it.physicalHeight) == maxOf(size.physicalWidth, size.physicalHeight) }
            .map { (it.refreshRate * 1000 + 0.5f).toInt() }.distinct().sortedDescending().ifEmpty { listOf(60000) }
        val rates = all.filter { cap == 0 || it <= cap * 1000 + 500 }.ifEmpty { listOf(all.last()) }
        return DisplayInfo(w, h, rates.toIntArray())
    }

    /** The refresh rates the display has, in Hz, for the settings screen. */
    fun displayRatesHz(): List<Int> {
        val display = if (Build.VERSION.SDK_INT >= 30) activity.display!! else @Suppress("DEPRECATION") activity.windowManager.defaultDisplay
        val size = display.mode
        return display.supportedModes.filter { maxOf(it.physicalWidth, it.physicalHeight) == maxOf(size.physicalWidth, size.physicalHeight) }
            .map { (it.refreshRate + 0.5f).toInt() }.distinct().sortedDescending()
    }

    private fun deviceCaps(): Int = 1 /* touch */ or (if (TouchForwarder.hasStylus()) 2 else 0)

    /** Contacts bigger than this are a palm: about 25 mm at this screen's density. */
    private fun palmSizePx(): Int = (25f / 25.4f * activity.resources.displayMetrics.densityDpi).toInt()

    companion object {
        const val ACTION_USB_PERMISSION = "dev.pdisp.USB_PERMISSION"
        // the native stages (session.c), and one of our own
        const val STAGE_PAIRING = 4
        const val STAGE_STREAMING = 6
        const val STAGE_RECONNECTING = 10
    }
}

/** The native side's words, turned into what happened and what to do about it. */
fun explain(text: String): Problem {
    val t = text.lowercase()
    return when {
        "busy with another device" in t -> Problem("The PC is busy", "It is already streaming to another device. Disconnect that one first, then connect again.")
        "connection reset" in t -> Problem("The PC closed the connection", "pdispd may have restarted or be busy with another device. Try again in a moment.")
        "cannot connect to" in t || "connection timed out" in t && "handshake" !in t ->
            Problem("Cannot reach the PC", "Check that the PC is on the same network, pdispd is running and the firewall lets port 47110 in.")
        "connection refused" in t -> Problem("The PC is not listening", "Start the pdisp service on the PC (systemctl --user start pdispd).")
        "handshake failed" in t -> Problem("The PC did not answer", "Over USB the daemon must run with the USB option (pdispd -u); over Wi-Fi check the address.")
        "does not use the secure protocol" in t -> Problem("The PC runs an older pdisp", "Update pdispd on the PC.")
        "refused" in t && "pairing was denied" in t -> Problem("Pairing was denied on the PC", "Connect again and choose Allow on the PC when it asks.")
        "pairing timed out" in t -> Problem("Nobody approved the pairing", "Connect again and answer the notification on the PC within 90 seconds.")
        "no video codec in common" in t -> Problem("No video codec in common", "Turn on the other codec in this app's Settings, or allow it on the PC: pdisp set codecs hevc,h264.")
        "the pc refused" in t -> Problem(text.replaceFirstChar { it.uppercase() }, null)
        "cannot decode" in t -> Problem("This device cannot decode the stream", "It needs a hardware HEVC or H.264 decoder.")
        "decoder failed" in t -> Problem("The video decoder failed", "Connect again. If it keeps happening, restart the app.")
        "lost contact" in t || "no data from the pc" in t -> Problem("Lost the connection to the PC", "Check the network or the cable.")
        "pc closed" in t || "pc ended" in t -> Problem("The PC ended the session", null)
        text.isBlank() -> Problem("Disconnected", null)
        else -> Problem(text.replaceFirstChar { it.uppercase() }, null)
    }
}
