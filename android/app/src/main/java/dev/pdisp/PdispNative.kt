package dev.pdisp

import android.view.Surface

/** The native session (src/main/cpp/session.c): handshake, loss-tolerant UDP receive and hardware decode onto a Surface. */
object PdispNative {
    init { System.loadLibrary("pdispnative") }

    external fun nativeStart(
        host: String, port: Int, surface: Surface,
        width: Int, height: Int, dpi: Int, refreshMhz: IntArray, name: String,
    ): Long

    /** Same session over a USB accessory file descriptor (ownership passes to the native side). */
    external fun nativeStartUsb(
        fd: Int, surface: Surface,
        width: Int, height: Int, dpi: Int, refreshMhz: IntArray, name: String,
    ): Long

    external fun nativeStop(handle: Long)
    external fun nativeStatus(handle: Long): String
    external fun nativeFinished(handle: Long): Boolean
}
