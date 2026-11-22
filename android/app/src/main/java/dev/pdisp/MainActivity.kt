package dev.pdisp

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.Configuration
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * Two ways in: add the PC's address (Wi-Fi), or plug the device into a PC that runs `pdispd -u`: the PC switches it to USB accessory mode,
 * Android starts this app, and the stream runs over the cable. While streaming the whole screen is the PC's monitor (a SurfaceView).
 */
class MainActivity : ComponentActivity() {
    private lateinit var store: Store
    private lateinit var session: SessionController
    private val touch = TouchForwarder()
    private lateinit var discovery: Discovery

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != SessionController.ACTION_USB_PERMISSION) return
            session.usbPermissionResult(accessoryFrom(intent), intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        store = Store(this)
        session = SessionController(this, store)
        discovery = Discovery(this)
        val filter = IntentFilter(SessionController.ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(permissionReceiver, filter)
        logDecoders()
        handleIntent(intent)
        setContent { PdispTheme { App() } }
    }

    private enum class Screen { Home, Settings }

    @Composable
    private fun App() {
        var pcs by remember { mutableStateOf(store.pcs()) }
        var settings by remember { mutableStateOf(store.settings) }
        var screen by remember { mutableStateOf(Screen.Home) }
        var menu by remember { mutableStateOf(false) }
        val phase = session.phase
        val view = LocalView.current

        // look for PCs on the network only while the list is showing
        val listing = phase == Phase.Idle && screen == Screen.Home
        DisposableEffect(listing) { if (listing) discovery.start(); onDispose { discovery.stop() } }

        // the screen stays on while connected, and the system bars go away while the picture is showing
        LaunchedEffect(phase, settings.keepScreenOn) {
            if (phase != Phase.Idle && settings.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            val c = WindowCompat.getInsetsController(window, view)
            c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            if (phase == Phase.Streaming) c.hide(WindowInsetsCompat.Type.systemBars()) else c.show(WindowInsetsCompat.Type.systemBars())
            if (phase != Phase.Streaming) menu = false
        }
        // full panel brightness (or the level the PC asked for) for this window only while the picture shows; the system brightness is never changed
        LaunchedEffect(phase, settings.fullBrightness, session.pcBrightness) {
            val lp = window.attributes
            val asked = session.pcBrightness // the PC set a level (DDC/CI): it wins over the switch
            lp.screenBrightness = if (phase != Phase.Streaming) WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
                else if (asked >= 0) (asked / 100f).coerceAtLeast(0.01f) // 0 would switch the panel off on some devices
                else if (settings.fullBrightness) WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_FULL else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            window.attributes = lp
        }
        LaunchedEffect(phase) { if (phase == Phase.Streaming) screen = Screen.Home; pcs = store.pcs() } // the list shows when each PC was last used

        BackHandler(enabled = phase != Phase.Idle || screen != Screen.Home) {
            when {
                menu -> menu = false
                phase == Phase.Streaming -> menu = true
                phase != Phase.Idle -> session.disconnect()
                else -> screen = Screen.Home
            }
        }

        Box(Modifier.fillMaxSize()) {
            // the picture: always there under everything, so the decoder has a surface the moment a session starts
            AndroidView(modifier = Modifier.fillMaxSize(), factory = { ctx ->
                SurfaceView(ctx).also { sv ->
                    sv.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(h: SurfaceHolder) = session.surfaceCreated(h.surface)
                        override fun surfaceChanged(h: SurfaceHolder, format: Int, width: Int, height: Int) {}
                        override fun surfaceDestroyed(h: SurfaceHolder) = session.surfaceDestroyed()
                    })
                    sv.setOnTouchListener { v, ev -> if (session.active && session.phase == Phase.Streaming) { touch.touch(session.handleOrZero, v, ev); true } else false }
                    sv.setOnHoverListener { v, ev -> if (session.active && session.phase == Phase.Streaming) { touch.hover(session.handleOrZero, v, ev); true } else false }
                }
            })

            when (phase) {
                Phase.Idle -> when (screen) {
                    Screen.Home -> HomeScreen(
                        session, pcs,
                        onConnect = { session.connect(it) },
                        onAdd = { host, name -> val pc = store.upsert(host, name); pcs = store.pcs(); session.connect(pc) },
                        onRename = { pc, n -> store.rename(pc.id, n); pcs = store.pcs() },
                        onForget = { pc -> store.remove(pc.id); pcs = store.pcs() },
                        onSettings = { screen = Screen.Settings },
                        found = discovery.found, onFound = { f -> val pc = store.upsert(f.address, f.name); pcs = store.pcs(); session.connect(pc) },
                    )
                    Screen.Settings -> SettingsScreen(
                        session, settings,
                        onChange = { settings = it; store.settings = it; session.settingsChanged() },
                        onForgetPairing = { store.forgetPairing() },
                        onBack = { screen = Screen.Home },
                    )
                }
                Phase.Connecting -> ConnectingScreen(session, onCancel = { session.disconnect() })
                Phase.Pairing -> PairingScreen(session, onCancel = { session.disconnect() })
                Phase.Streaming -> {
                    if (settings.showStats) StatsOverlay(session.stats)
                    if (menu) StreamMenu(session, settings, onChange = { settings = it; store.settings = it }, onDismiss = { menu = false },
                        onDisconnect = { menu = false; session.disconnect() })
                }
            }
        }
    }

    /** The phone was rotated: tell the PC, which rebuilds its virtual monitor at the new size (RECONFIGURE). */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        session.displayChanged()
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
        val usb = getSystemService(Context.USB_SERVICE) as UsbManager
        val acc = (if (intent?.action == UsbManager.ACTION_USB_ACCESSORY_ATTACHED) accessoryFrom(intent) else null) ?: usb.accessoryList?.firstOrNull()
        if (acc != null) session.connectUsb(acc)
    }

    /** One-time diagnostics: which HEVC decoders exist, whether they claim low latency, and their vendor parameters. */
    private fun logDecoders() {
        for (info in MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos) {
            if (info.isEncoder || !info.supportedTypes.contains("video/hevc")) continue
            val caps = info.getCapabilitiesForType("video/hevc")
            Log.i("pdisp", "decoder ${info.name} hw=${info.isHardwareAccelerated} lowLatency=${caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)} " +
                "maxInstances=${caps.maxSupportedInstances} 2304x1440@60=${caps.videoCapabilities?.areSizeAndRateSupported(2304, 1440, 60.0)}")
            if (Build.VERSION.SDK_INT >= 33 && info.isHardwareAccelerated) {
                try {
                    val c = MediaCodec.createByCodecName(info.name)
                    Log.i("pdisp", "  vendor params: ${c.supportedVendorParameters}")
                    c.release()
                } catch (e: Exception) { Log.i("pdisp", "  vendor params: $e") }
            }
        }
    }
}
