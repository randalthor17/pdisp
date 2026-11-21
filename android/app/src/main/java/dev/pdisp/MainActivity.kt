package dev.pdisp

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log
import android.hardware.usb.UsbManager
import android.net.wifi.WifiManager
import android.app.KeyguardManager
import android.os.Build
import android.os.PowerManager
import kotlin.math.cos
import kotlin.math.sin
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.InputDevice
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/**
 * A full-screen SurfaceView with a small overlay. Two ways in: type the PC's address (Wi-Fi), or plug the tablet into a PC that
 * runs `pdispd -u`: the PC switches it to USB accessory mode, Android starts this app, and the stream runs over the cable.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: LinearLayout
    private lateinit var hostField: EditText
    private lateinit var status: TextView
    private lateinit var button: Button
    private var wifiLock: WifiManager.WifiLock? = null
    private var handle = 0L
    private var surfaceReady = false
    private var pendingAccessory: UsbAccessory? = null

    // What the user wants, which outlives any one session: while wantConnected is set the app keeps (re)connecting to `target`
    private sealed class Target { class Host(val host: String) : Target(); object Usb : Target() }
    private var wantConnected = false
    private var target: Target? = null
    private var attempts = 0
    private val reconnect = Runnable { connectNow() }
    private val ui = Handler(Looper.getMainLooper())
    private val usb by lazy { getSystemService(Context.USB_SERVICE) as UsbManager }

    private val poll = object : Runnable {
        override fun run() {
            if (handle != 0L) {
                val text = PdispNative.nativeStatus(handle)
                if (PdispNative.nativeFinished(handle)) sessionEnded(text)
                else {
                    status.text = text
                    val streaming = streamingText.containsMatchIn(text)
                    if (streaming) attempts = 0
                    showOverlay(!streaming)
                    ui.postDelayed(this, 500)
                }
            }
        }
    }

    private val streamingText = Regex("^\\d+x\\d+ +\\d+ fps")

    /** While the picture is showing, the whole screen is the PC's monitor: the overlay would swallow touches and cover the corner. */
    private fun showOverlay(show: Boolean) { overlay.visibility = if (show) View.VISIBLE else View.GONE }

    override fun onBackPressed() {
        if (handle != 0L || wantConnected) { disconnect() } else super.onBackPressed() // Back ends the stream first, then leaves the app
    }

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val acc = accessoryFrom(intent)
            if (acc != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) connectUsb(acc)
            else status.text = "USB access was not allowed"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this).also {
            it.holder.addCallback(this)
            it.setOnTouchListener { v, ev -> if (handle != 0L) { sendTouch(v, ev); true } else false }
            it.setOnHoverListener { v, ev -> if (handle != 0L) { sendHover(v, ev); true } else false }
        }
        root.addView(surfaceView)

        hostField = EditText(this).apply {
            hint = "PC address, e.g. 192.168.1.20 (or plug in a USB cable)"
            setText(getPreferences(Context.MODE_PRIVATE).getString("host", ""))
            setSingleLine()
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
            // no full-screen keyboard (it hides the rest of the UI in landscape), and its Go key connects
            imeOptions = EditorInfo.IME_ACTION_GO or EditorInfo.IME_FLAG_NO_EXTRACT_UI
            setOnEditorActionListener { v, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_GO) {
                    (getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(v.windowToken, 0)
                    if (handle == 0L) startSession()
                    true
                } else false
            }
        }
        status = TextView(this).apply { setTextColor(Color.WHITE); textSize = 14f }
        button = Button(this).apply { text = "Connect"; setOnClickListener { toggle() } }
        overlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xAA000000.toInt())
            setPadding(32, 24, 32, 24)
            addView(hostField); addView(button); addView(status)
        }
        root.addView(overlay, FrameLayout.LayoutParams(900, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
        setContentView(root)
        hideSystemBars()
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(permissionReceiver, filter)
        logDecoders()
        handleIntent(intent)
    }

    /** One-time diagnostics: which HEVC decoders exist, whether they claim low latency, and their vendor parameters. */
    private fun logDecoders() {
        for (info in MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos) {
            if (info.isEncoder || !info.supportedTypes.contains("video/hevc")) continue
            val caps = info.getCapabilitiesForType("video/hevc")
            Log.i("pdisp", "decoder ${info.name} hw=${info.isHardwareAccelerated} lowLatency=${caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)} " +
                "maxInstances=${caps.maxSupportedInstances} 2304x1440@60=${caps.videoCapabilities.areSizeAndRateSupported(2304, 1440, 60.0)}")
            if (Build.VERSION.SDK_INT >= 33 && info.isHardwareAccelerated) {
                try {
                    val c = MediaCodec.createByCodecName(info.name)
                    Log.i("pdisp", "  vendor params: ${c.supportedVendorParameters}")
                    c.release()
                } catch (e: Exception) { Log.i("pdisp", "  vendor params: $e") }
            }
        }
    }

    private val lastSent = HashMap<Int, Pair<Int, Int>>() // pointer id -> last position sent, so a still finger sends nothing
    private var penNear = false

    /** Does this phone or tablet have a pen digitiser (an S Pen, a Pixel Tablet pen, ...)? Then the PC also gets a pen tablet. */
    private fun hasStylus(): Boolean = InputDevice.getDeviceIds().any { id ->
        val d = InputDevice.getDevice(id)
        d != null && (d.sources and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS
    }

    private fun deviceCaps(): Int = 1 /* touch */ or (if (hasStylus()) 2 else 0)

    /** Contacts bigger than this are a palm: about 25 mm at this screen's density. */
    private fun palmSizePx(): Int = (25f / 25.4f * resources.displayMetrics.densityDpi).toInt()

    /** Touches and pen events on the stream go to the PC as the events of a touchscreen and a pen tablet; the PC's compositor does the
     *  gestures. Palm rejection happens in the native session before anything is sent. */
    private fun sendTouch(v: View, ev: MotionEvent) {
        val w = v.width.toFloat().coerceAtLeast(1f)
        val h = v.height.toFloat().coerceAtLeast(1f)
        val canceled = (ev.flags and 0x20) != 0 // MotionEvent.FLAG_CANCELED (API 33)
        fun px(i: Int) = (ev.getX(i) / w * 65535f).toInt().coerceIn(0, 65535)
        fun py(i: Int) = (ev.getY(i) / h * 65535f).toInt().coerceIn(0, 65535)
        fun pr(i: Int) = (ev.getPressure(i).coerceIn(0f, 1f) * 65535f).toInt()
        fun tool(i: Int) = when (ev.getToolType(i)) {
            MotionEvent.TOOL_TYPE_STYLUS -> 1
            MotionEvent.TOOL_TYPE_ERASER -> 2
            else -> 0
        }
        fun flags(i: Int) = (if (ev.getToolType(i) == 5 /* TOOL_TYPE_PALM, API 33 */) 1 else 0) or (if (canceled) 2 else 0)
        fun buttons() = (if ((ev.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) 1 else 0) or
            (if ((ev.buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY) != 0) 2 else 0)
        // the pen's tilt, as the two angles a pen tablet reports: AXIS_TILT is the angle from vertical, AXIS_ORIENTATION its direction
        fun tiltX(i: Int) = Math.toDegrees(ev.getAxisValue(MotionEvent.AXIS_TILT, i).toDouble() * sin(ev.getOrientation(i).toDouble())).toInt()
        fun tiltY(i: Int) = Math.toDegrees(ev.getAxisValue(MotionEvent.AXIS_TILT, i).toDouble() * cos(ev.getOrientation(i).toDouble())).toInt()
        fun send(kind: Int, i: Int) {
            val id = ev.getPointerId(i)
            val x = px(i); val y = py(i)
            val t = tool(i)
            if (kind == 1 && t == 0 && lastSent[id] == Pair(x, y)) return
            lastSent[id] = Pair(x, y)
            val pen = t != 0
            PdispNative.nativeTouch(
                handle, kind, id, x, y, pr(i), t, if (pen) buttons() else 0, if (pen) tiltX(i) else 0, if (pen) tiltY(i) else 0,
                (ev.getTouchMajor(i)).toInt(), flags(i),
            )
        }
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> send(0, ev.actionIndex)
            MotionEvent.ACTION_MOVE -> for (i in 0 until ev.pointerCount) send(1, i)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> { send(2, ev.actionIndex); lastSent.remove(ev.getPointerId(ev.actionIndex)) }
            MotionEvent.ACTION_CANCEL -> { for (i in 0 until ev.pointerCount) send(3, i); lastSent.clear() }
        }
    }

    /** The pen hovering over the screen (not touching): the PC's pen tablet reports it in proximity with its distance. */
    private fun sendHover(v: View, ev: MotionEvent) {
        val i = 0
        val t = when (ev.getToolType(i)) { MotionEvent.TOOL_TYPE_STYLUS -> 1; MotionEvent.TOOL_TYPE_ERASER -> 2; else -> return }
        val w = v.width.toFloat().coerceAtLeast(1f)
        val h = v.height.toFloat().coerceAtLeast(1f)
        val x = (ev.getX(i) / w * 65535f).toInt().coerceIn(0, 65535)
        val y = (ev.getY(i) / h * 65535f).toInt().coerceIn(0, 65535)
        val buttons = (if ((ev.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) 1 else 0) or
            (if ((ev.buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY) != 0) 2 else 0)
        when (ev.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                val distance = (ev.getAxisValue(MotionEvent.AXIS_DISTANCE, i).coerceIn(0f, 1f) * 65535f).toInt()
                val tilt = Math.toDegrees(ev.getAxisValue(MotionEvent.AXIS_TILT, i).toDouble())
                val o = ev.getOrientation(i).toDouble()
                penNear = true
                PdispNative.nativeTouch(handle, 4, ev.getPointerId(i), x, y, distance, t, buttons, (tilt * sin(o)).toInt(), (tilt * cos(o)).toInt(), 0, 0)
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                if (penNear) PdispNative.nativeTouch(handle, 3, ev.getPointerId(i), x, y, 0, t, 0, 0, 0, 0, 0) // the pen left
                penNear = false
            }
        }
    }

    /** The phone was rotated: tell the PC, which rebuilds its virtual monitor at the new size (RECONFIGURE). */
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        if (handle != 0L) {
            val d = displayInfo()
            PdispNative.nativeDisplayChanged(handle, d.width, d.height)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onDestroy() {
        unregisterReceiver(permissionReceiver)
        super.onDestroy()
    }

    private fun accessoryFrom(intent: Intent): UsbAccessory? =
        if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
        else @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY)

    /** Started by plugging in (the PC switched this device to accessory mode), or opened by hand while a cable is already in. */
    private fun handleIntent(intent: Intent?) {
        val acc = (if (intent?.action == UsbManager.ACTION_USB_ACCESSORY_ATTACHED) accessoryFrom(intent) else null)
            ?: usb.accessoryList?.firstOrNull()
        if (acc != null) connectUsb(acc)
    }

    private fun connectUsb(acc: UsbAccessory) {
        wantConnected = true
        target = Target.Usb
        if (handle != 0L) return
        if (!usb.hasPermission(acc)) {
            val flags = PendingIntent.FLAG_MUTABLE // the system adds the result extras
            val pi = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION).setPackage(packageName), flags)
            usb.requestPermission(acc, pi)
            status.text = "waiting for USB permission"
            return
        }
        if (!surfaceReady) { pendingAccessory = acc; status.text = "USB connected, preparing the display"; return }
        pendingAccessory = null
        val pfd = usb.openAccessory(acc)
        if (pfd == null) { status.text = "cannot open the USB accessory"; return }
        val d = displayInfo()
        handle = PdispNative.nativeStartUsb(
            pfd.detachFd(), surfaceView.holder.surface, d.width, d.height, resources.displayMetrics.densityDpi, d.rates, Build.MODEL,
            filesDir.absolutePath, deviceCaps(),
        )
        sessionStarted()
    }

    private fun hideSystemBars() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
    }

    private fun toggle() {
        if (handle != 0L || wantConnected) disconnect() else startSession()
    }

    /** The user ends the connection: no reconnecting, and the PC removes the monitor at once. */
    private fun disconnect() {
        wantConnected = false
        ui.removeCallbacks(reconnect)
        stopSession(0)
    }

    /** Why we are leaving, as the PC should hear it: the screen locking or turning off is not the same as the user ending the stream. */
    private fun closeReason(): Int {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        val km = getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        return if (!pm.isInteractive || km.isKeyguardLocked) 1 else 0
    }

    /** (Re)connects to what the user last asked for; called at start, on resume and by the retry timer. */
    private fun connectNow() {
        if (!wantConnected || handle != 0L || !surfaceReady) return
        when (val t = target) {
            is Target.Host -> startHostSession(t.host)
            Target.Usb -> usb.accessoryList?.firstOrNull()?.let { connectUsb(it) } ?: run { status.text = "waiting for the USB cable"; scheduleReconnect() }
            null -> {}
        }
    }

    private fun scheduleReconnect() {
        ui.removeCallbacks(reconnect)
        attempts++
        ui.postDelayed(reconnect, minOf(1000L * attempts, 5000L))
    }

    /** The native session ended by itself: tidy up, and try again if the link failed and the user still wants the connection. */
    private fun sessionEnded(message: String) {
        val kind = PdispNative.nativeEndKind(handle)
        stopSession(3)
        if (wantConnected && kind == 1) {
            status.text = "lost the PC, reconnecting (attempt $attempts): $message"
            scheduleReconnect()
        } else {
            if (kind == 2) wantConnected = false // refused or impossible: retrying would only repeat it
            status.text = message
        }
    }

    private class DisplayInfo(val width: Int, val height: Int, val rates: IntArray)

    /** The display's real pixel size (landscape, rounded down to a multiple of 8 as the encoder needs) and its refresh rates in mHz. */
    private fun displayInfo(): DisplayInfo {
        val b = windowManager.currentWindowMetrics.bounds // follows the current rotation (this activity is full screen)
        val w = b.width() and 7.inv()
        val h = b.height() and 7.inv()
        val display = if (Build.VERSION.SDK_INT >= 30) display!! else @Suppress("DEPRECATION") windowManager.defaultDisplay
        val size = display.mode
        val rates = display.supportedModes
            .filter { maxOf(it.physicalWidth, it.physicalHeight) == maxOf(size.physicalWidth, size.physicalHeight) }
            .map { (it.refreshRate * 1000 + 0.5f).toInt() }.distinct().sortedDescending().ifEmpty { listOf(60000) }
        return DisplayInfo(w, h, rates.toIntArray())
    }

    private fun startSession() {
        val host = hostField.text.toString().trim()
        if (host.isEmpty() || !surfaceReady) return
        getPreferences(Context.MODE_PRIVATE).edit().putString("host", host).apply()
        wantConnected = true
        target = Target.Host(host)
        attempts = 0
        startHostSession(host)
    }

    private fun startHostSession(host: String) {
        val d = displayInfo()
        handle = PdispNative.nativeStart(
            host, 47110, surfaceView.holder.surface, d.width, d.height, resources.displayMetrics.densityDpi, d.rates, Build.MODEL,
            filesDir.absolutePath, deviceCaps(),
        )
        sessionStarted()
    }

    private fun sessionStarted() {
        if (handle == 0L) return
        PdispNative.nativeSetPalmRejection(handle, true, palmSizePx())
        // match the panel's refresh to the stream: 60 fps on a 90 Hz panel lands on uneven vsync slots
        window.attributes = window.attributes.also { it.preferredRefreshRate = 60f }
        // Wi-Fi power save batches packets for tens of milliseconds, which shows up as stutter
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "pdisp").also { it.acquire() }
        button.text = "Disconnect"
        ui.post(poll)
    }

    private fun stopSession(reason: Int) {
        val h = handle
        handle = 0
        if (h != 0L) PdispNative.nativeStop(h, reason)
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        button.text = "Connect"
        status.text = "disconnected"
        showOverlay(true)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        pendingAccessory?.let { connectUsb(it) } ?: connectNow() // coming back to the app (after the screen was off): pick up where we were
    }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    /** Leaving the screen (the screen turned off, a lock, another app): end the stream, saying why, but remember that we want it back. */
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        ui.removeCallbacks(reconnect)
        stopSession(closeReason())
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) hideSystemBars() }

    companion object { private const val ACTION_USB_PERMISSION = "dev.pdisp.USB_PERMISSION" }
}
