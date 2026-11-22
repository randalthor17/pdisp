package dev.pdisp

import android.view.Surface

/** The native session (src/main/cpp/session.c): handshake, loss-tolerant UDP receive and hardware decode onto a Surface. */
object PdispNative {
    init { System.loadLibrary("pdispnative") }

    external fun nativeStart(
        host: String, port: Int, surface: Surface,
        width: Int, height: Int, dpi: Int, refreshMhz: IntArray, name: String, identityDir: String, caps: Int,
    ): Long

    /** Same session over a USB accessory file descriptor (ownership passes to the native side). */
    external fun nativeStartUsb(
        fd: Int, surface: Surface,
        width: Int, height: Int, dpi: Int, refreshMhz: IntArray, name: String, identityDir: String, caps: Int,
    ): Long

    /** The display size changed (rotation): the session reports it to the PC. */
    external fun nativeDisplayChanged(handle: Long, width: Int, height: Int)

    /** One contact: kind 0 down, 1 move, 2 up, 3 cancel (for a pen: it left), 4 hover (pressure = distance); x, y in 0..65535 across the
     *  screen; tool 0 finger, 1 stylus, 2 eraser; buttons bit 0 barrel, bit 1 second; tilt in degrees; sizePx the contact's major axis;
     *  flags 1 = the system calls it a palm, 2 = the system cancelled it. */
    external fun nativeTouch(
        handle: Long, kind: Int, pointerId: Int, x: Int, y: Int, pressure: Int, tool: Int,
        buttons: Int, tiltX: Int, tiltY: Int, sizePx: Int, flags: Int,
    )

    /** Palm rejection on or off, and the contact size in pixels above which a contact is a palm. */
    external fun nativeSetPalmRejection(handle: Long, enabled: Boolean, palmSizePx: Int)

    /** Stops the session and tells the PC why: 0 on purpose, 1 the screen locked, 3 error. */
    external fun nativeStop(handle: Long, reason: Int)

    /** Why a finished session ended: 0 we stopped it, 1 the link failed (try again), 2 refused or impossible (do not retry). */
    external fun nativeEndKind(handle: Long): Int
    /** int[11]: stage (STAGE_*), the pairing code, then width, height, fps x 10, decoded, lost frames, IDR requests, touches sent, touches acknowledged, and the brightness the PC asked for (0..100, -1 for none). */
    external fun nativeInfo(handle: Long): IntArray
    external fun nativeStatus(handle: Long): String
    external fun nativeFinished(handle: Long): Boolean
}
