package dev.pdisp

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
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
    private val ui = Handler(Looper.getMainLooper())
    private val usb by lazy { getSystemService(Context.USB_SERVICE) as UsbManager }

    private val poll = object : Runnable {
        override fun run() {
            if (handle != 0L) {
                val text = PdispNative.nativeStatus(handle)
                if (PdispNative.nativeFinished(handle)) { stopSession(); status.text = text } // it ended or failed: keep its message
                else { status.text = text; ui.postDelayed(this, 500) }
            }
        }
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
        surfaceView = SurfaceView(this).also { it.holder.addCallback(this) }
        root.addView(surfaceView)

        hostField = EditText(this).apply {
            hint = "PC address, e.g. 192.168.1.20 (or plug in a USB cable)"
            setText(getPreferences(Context.MODE_PRIVATE).getString("host", ""))
            setSingleLine()
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
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
        handleIntent(intent)
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
        )
        sessionStarted()
    }

    private fun hideSystemBars() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
    }

    private fun toggle() {
        if (handle != 0L) stopSession() else startSession()
    }

    private class DisplayInfo(val width: Int, val height: Int, val rates: IntArray)

    /** The display's real pixel size (landscape, rounded down to a multiple of 8 as the encoder needs) and its refresh rates in mHz. */
    private fun displayInfo(): DisplayInfo {
        val b = windowManager.maximumWindowMetrics.bounds
        val w = maxOf(b.width(), b.height()) and 7.inv()
        val h = minOf(b.width(), b.height()) and 7.inv()
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
        val d = displayInfo()
        handle = PdispNative.nativeStart(
            host, 47110, surfaceView.holder.surface, d.width, d.height, resources.displayMetrics.densityDpi, d.rates, Build.MODEL,
        )
        sessionStarted()
    }

    private fun sessionStarted() {
        if (handle == 0L) return
        // match the panel's refresh to the stream: 60 fps on a 90 Hz panel lands on uneven vsync slots
        window.attributes = window.attributes.also { it.preferredRefreshRate = 60f }
        // Wi-Fi power save batches packets for tens of milliseconds, which shows up as stutter
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "pdisp").also { it.acquire() }
        button.text = "Disconnect"
        ui.post(poll)
    }

    private fun stopSession() {
        val h = handle
        handle = 0
        if (h != 0L) PdispNative.nativeStop(h)
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        button.text = "Connect"
        status.text = "disconnected"
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        pendingAccessory?.let { connectUsb(it) }
    }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) { surfaceReady = false; stopSession() }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) hideSystemBars() }

    companion object { private const val ACTION_USB_PERMISSION = "dev.pdisp.USB_PERMISSION" }
}
