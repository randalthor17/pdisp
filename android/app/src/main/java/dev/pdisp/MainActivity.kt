package dev.pdisp

import android.app.Activity
import android.content.Context
import android.graphics.Color
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
 * M5 skeleton: a full-screen SurfaceView, and an overlay to type the PC's address. The PC creates a virtual monitor at this
 * display's native size and the stream is decoded straight onto the surface. Pairing (M6) and USB (M7) come later.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: LinearLayout
    private lateinit var hostField: EditText
    private lateinit var status: TextView
    private var handle = 0L
    private var surfaceReady = false
    private val ui = Handler(Looper.getMainLooper())
    private val poll = object : Runnable {
        override fun run() {
            if (handle != 0L) {
                val text = PdispNative.nativeStatus(handle)
                if (PdispNative.nativeFinished(handle)) { stopSession(); status.text = text } // it ended or failed: keep its message
                else { status.text = text; ui.postDelayed(this, 500) }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this).also { it.holder.addCallback(this) }
        root.addView(surfaceView)

        hostField = EditText(this).apply {
            hint = "PC address, e.g. 192.168.1.20"
            setText(getPreferences(Context.MODE_PRIVATE).getString("host", ""))
            setSingleLine()
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
        }
        status = TextView(this).apply { setTextColor(Color.WHITE); textSize = 14f }
        val button = Button(this).apply { text = "Connect"; setOnClickListener { toggle() } }
        overlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xAA000000.toInt())
            setPadding(32, 24, 32, 24)
            addView(hostField); addView(button); addView(status)
        }
        root.addView(overlay, FrameLayout.LayoutParams(900, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
        setContentView(root)
        hideSystemBars()
    }

    private fun hideSystemBars() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
    }

    private fun toggle() {
        if (handle != 0L) stopSession() else startSession()
    }

    /** The display's real pixel size, rounded down to a multiple of 8 (the encoder needs that). */
    private fun nativeSize(): Pair<Int, Int> {
        val b = windowManager.maximumWindowMetrics.bounds
        val w = maxOf(b.width(), b.height()) // the app is locked to landscape
        val h = minOf(b.width(), b.height())
        return Pair(w and 7.inv(), h and 7.inv())
    }

    private fun startSession() {
        val host = hostField.text.toString().trim()
        if (host.isEmpty() || !surfaceReady) return
        getPreferences(Context.MODE_PRIVATE).edit().putString("host", host).apply()
        val (w, h) = nativeSize()
        val display = if (Build.VERSION.SDK_INT >= 30) display!! else @Suppress("DEPRECATION") windowManager.defaultDisplay
        val size = display.mode
        // every refresh rate this display offers at its native resolution, in millihertz
        val rates = display.supportedModes
            .filter { maxOf(it.physicalWidth, it.physicalHeight) == maxOf(size.physicalWidth, size.physicalHeight) }
            .map { (it.refreshRate * 1000 + 0.5f).toInt() }.distinct().sortedDescending().ifEmpty { listOf(60000) }
        handle = PdispNative.nativeStart(
            host, 47110, surfaceView.holder.surface, w, h, resources.displayMetrics.densityDpi,
            rates.toIntArray(), Build.MODEL,
        )
        if (handle != 0L) { (overlay.getChildAt(1) as Button).text = "Disconnect"; ui.post(poll) }
    }

    private fun stopSession() {
        val h = handle
        handle = 0
        if (h != 0L) PdispNative.nativeStop(h)
        (overlay.getChildAt(1) as Button).text = "Connect"
        status.text = "disconnected"
    }

    override fun surfaceCreated(holder: SurfaceHolder) { surfaceReady = true }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) { surfaceReady = false; stopSession() }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) hideSystemBars() }
}
