# Spike: can this GPU do low-latency H.264 with our loss recovery? (2026-11-23)

Question: before building an H.264 path beside HEVC, does VAAPI on the RX 9060 XT (Mesa 26.3.0, libva 1.24, kernel 6.18.64) encode H.264 at all, and can it
do what the loss recovery needs: predicting from an older frame than the previous one, with the stream still decoding bit-exactly after frames are lost?

1. Plain support: `ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i testsrc2=size=1280x800:rate=90 -t 1 -vf format=nv12,hwupload -c:v h264_vaapi -bf 0 -f h264 out.h264`
   works on both render nodes. The driver reports CQP, CBR, VBR and QVBR, packed headers 0x1f (sequence, picture, slice, misc, raw), one reference in P.
   `-bsf:v trace_headers` on that output gave the template for the SPS, PPS and slice headers (High profile, CABAC, 8x8 transform, POC type 2).
2. Our own libva code (`daemon/h264_headers.c`, `daemon/encoder.c`, driven by `tests/enc_synth -C h264`): the first encode decoded without a warning (30 of 30 frames).
3. The question that mattered: `tests/loss_recovery_check.sh build/enc_synth DIR h264` (240 frames of scrolling content, frames removed from the stream before decoding):

       ok: short_loss (lost frames 100-103, next frame predicts 5 back, 236 frames identical)
       ok: max_loss (lost frames 100-104, next frame predicts 6 back, 235 frames identical)
       ok: long_loss (lost frames 100-110, next frame predicts 0 back, 229 frames identical)

   Bit-identical to the clean stream with the same frames skipped, with no decoder error. That needs `gaps_in_frame_num_value_allowed_flag = 1` and a reference list
   modification in the P slice header; both are in `docs/protocol.md` section 2.4.

Result: yes. The details of what the driver does with the headers are in `docs/encoder-notes.md`. Open: a phone's hardware decoder with frame_num gaps (the ffmpeg decoder is
not MediaCodec), and the bitrate and latency at 90 Hz against HEVC.
