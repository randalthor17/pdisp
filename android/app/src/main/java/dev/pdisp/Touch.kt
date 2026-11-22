package dev.pdisp

import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import kotlin.math.cos
import kotlin.math.sin

/**
 * Touches and pen events on the stream go to the PC as the events of a touchscreen and a pen tablet; the PC's compositor does the gestures.
 * Palm rejection happens in the native session before anything is sent.
 */
class TouchForwarder {
    private val lastSent = HashMap<Int, Pair<Int, Int>>() // pointer id -> last position sent, so a still finger sends nothing
    private var penNear = false

    companion object {
        /** Does this phone or tablet have a pen digitiser (an S Pen, a Pixel Tablet pen, ...)? Then the PC also gets a pen tablet. */
        fun hasStylus(): Boolean = InputDevice.getDeviceIds().any { id ->
            val d = InputDevice.getDevice(id)
            d != null && (d.sources and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS
        }
    }

    fun touch(handle: Long, v: View, ev: MotionEvent) {
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
    fun hover(handle: Long, v: View, ev: MotionEvent) {
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
}
