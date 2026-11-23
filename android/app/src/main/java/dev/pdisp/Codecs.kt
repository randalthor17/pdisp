package dev.pdisp

import android.media.MediaCodecList

/** The video codecs this device can offer the PC. The numbers are the protocol's codec ids (proto.h: PROTO_CODEC_HEVC, PROTO_CODEC_H264). */
object Codecs {
    const val HEVC = 1
    const val H264 = 4

    private fun hardwareDecoder(mime: String): Boolean =
        MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.any { info ->
            !info.isEncoder && info.isHardwareAccelerated && info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
        }

    /** Whether a hardware decoder exists: only those are offered, a software decoder cannot keep up with a screen. */
    val hevcHardware: Boolean by lazy { hardwareDecoder("video/hevc") }
    val h264Hardware: Boolean by lazy { hardwareDecoder("video/avc") }

    /** The codecs to say the device can decode, as a mask with bit `id` set for each (what native HELLO lists): what the settings ask for, within
     *  what the device has. If that leaves nothing, whatever the device has; if it has nothing in hardware, both (a software decoder may do). */
    fun mask(s: Settings): Int {
        var m = 0
        if (s.hevc && hevcHardware) m = m or (1 shl HEVC)
        if (s.h264 && h264Hardware) m = m or (1 shl H264)
        if (m == 0) {
            if (hevcHardware) m = m or (1 shl HEVC)
            if (h264Hardware) m = m or (1 shl H264)
        }
        if (m == 0) m = (1 shl HEVC) or (1 shl H264)
        return m
    }
}
