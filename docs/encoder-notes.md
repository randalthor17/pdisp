# Encoder notes (M3)

Direct libva HEVC encode on the RX 9060 XT (Mesa radeonsi 26.3.0, libva 1.24, kernel 6.18.63).
Code: `daemon/encoder.c`, `daemon/hevc_headers.c`. Check: `tests/m3_encode_check.sh`.

## Pipeline per frame

1. Upload the BGRA frame to a `VA_RT_FORMAT_RGB32` (BGRX) surface (`vaCreateImage` + `vaPutImage`): ~1.5 to 2.2 ms for 2304x1440.
2. Convert to NV12 on the GPU with the VA video-processing pipeline: BT.709, full range in, limited range out:
   ~0.8 ms.
3. Encode HEVC Main 8-bit: I (first frame and on request) then P, one reference (`l0=1` is the driver limit),
   one slice, no B-frames: ~1.9 to 3.8 ms (varies run to run, probably GPU clocks).
4. `vaSyncSurface`, then map the coded buffer.

Everything is synchronous, so about 5 ms per frame end to end at 2304x1440. It fits a 60 Hz budget but pipelining
(upload of frame N+1 while N encodes) is a later optimisation.

## Mesa wants the application to supply the headers

This is the finding that cost the most time. Without packed headers the driver emits **no VPS/SPS/PPS at all** (the
stream starts at the slice and decoders report `PPS id out of range`). Reading Mesa
(`src/gallium/frontends/va/picture_hevc_enc.c`, `src/gallium/drivers/radeonsi/mm/radeon_vcn_enc.c`):

- The application sends packed headers (`VAEncPackedHeaderSequence` carrying VPS+SPS+PPS, and
  `VAEncPackedHeaderHEVC_Slice` for each slice). Mesa **parses** them to learn the stream parameters.
- The encoder emits headers only if there is at least one slice header **and** at least one other header
  (`if (!num_headers || !num_slices || num_headers == num_slices) return NULL;`). A sequence header alone is
  silently dropped. Both config attributes must be requested:
  `VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE`.
- Mesa then **rewrites** VPS/SPS/PPS with its own writer from the parsed values, so our SPS must be valid and agree with
  the VA sequence/picture parameters (32x32 CTU, 8x8 min CB, AMP on, SAO and temporal MVP off, 12-bit POC LSB,
  `cu_qp_delta` on). The VUI (limited range, BT.709 primaries/matrix, sRGB transfer) survives the rewrite;
  `ffprobe` reports `color_range=tv color_space=bt709 color_primaries=bt709 color_transfer=iec61966-2-1`.
- The slice header we send is parsed for POC/RPS information. The picture data and the final slice header come from
  the hardware. P-frames use an explicit one-picture RPS (`num_negative_pics=1`, previous picture).
- Packed headers use `has_emulation_bytes = 1`: our bytes already contain emulation prevention.

## Limits and constants

- Width and height must be multiples of 8 (min CB 8, no conformance window yet). A mode like 2340x1080 needs a
  conformance window (crop) and an SPS that Mesa accepts with it. Not done.
- Level is chosen by luma sample rate: 5.0 up to 267 Msamples/s, 5.1 up to 535, 5.2 above.
- Seven reconstruction surfaces form a retained-reference buffer (the current picture plus up to six references); every
  slice header lists the valid ones, one used. This allows reference invalidation and an optional rolling intra refresh
  and multi-slice pictures. See `docs/spikes/m0-loss-resilience.md`.

## Rate control (desktop content, 2304x1440@60, animated test pattern, `-k 60` forces an IDR per second)

| Mode | Average | I avg / max | P avg / max |
|---|---|---|---|
| CBR 20 Mbit/s, 30 ms buffer | 19.2 Mbit/s | 66 / 80 KB | 41 / 94 KB |
| VBR peak 20 Mbit/s, 50% target, 30 ms buffer | 9.3 Mbit/s | 38 / 44 KB | 20 / 117 KB |

- CBR pads every P-frame to the target (41.7 KB at 20 Mbit/s and 60 fps) even when little changed, which wastes link
  bandwidth on a mostly static desktop. VBR with a peak cap is the default.
- A 30 ms buffer keeps keyframes at or below the largest P-frame in both modes (matches the ffmpeg result in
  `docs/spikes/m0-virtual-output.md`). The 117 KB P-frame peak in VBR is the largest frame of the run and still
  within the buffer; its cause (scene change when the pattern wraps) is not analysed.
- Only the first frame is an IDR unless asked. The first frame the capture delivers can be the wallpaper rather than
  the pattern, so it is cheap (4 KB); that is why the first IDR is tiny.

## Colour check

Decoded frame vs the exact pattern, away from the moving bar: channel means differ by 3.3 (R), 4.5 (G) and 0.4 (B) levels
out of 255. That rules out a matrix or range mix-up; the remaining offset is quantisation and 4:2:0 chroma on the hard
checkerboard edges (PSNR 28.7 dB on that deliberately brutal pattern, which is not a quality measure for text).
Text quality is an M10 item.

## Not covered yet

- Pipelining and ROI. Intra refresh runs but its healing is unverified; temporal layers are incompatible with it
  and unused.
- Reconnect and mode change: the encoder is opened for one size and the tool ignores frames of another size (M9).
- The Android decoder accepting this exact stream (level 5.0/5.1, tv range) is verified only on the desktop (M5).

## H.264 (the same encoder, a second codec)

`encoder_config.codec` selects HEVC (default) or H.264. Code: `daemon/h264_headers.c` (SPS, PPS, slice header), the `_h264` functions in `daemon/encoder.c`.
Checks: `tests/test_h264_headers.c`, `tests/loss_recovery_check.sh <enc_synth> <dir> h264`, `tests/m14_codec_check.sh`, and the m4 loopback test with `-F h264`.
The stream properties are in `docs/protocol.md` (section 2.4). What was found building it (RX 9060 XT, Mesa 26.3, libva 1.24):

- **Same rule as HEVC: the application supplies the headers.** The packed sequence header carries SPS and PPS together, and every slice has a packed slice header. The driver
  passes our SPS through into the stream unchanged (checked with ffmpeg's `trace_headers`: `gaps_in_frame_num_allowed_flag`, `max_num_ref_frames`, the VUI), unlike HEVC where Mesa rewrites it.
- **The CABAC slice header ends with `cabac_alignment_one_bit`** (ones to the byte boundary), not the 1-then-zeros trailing bits of HEVC. The bit length of the packed slice header includes them.
- **Loss recovery is bit-exact**, as with HEVC: after a loss the next frame predicts from the newest frame the receiver has (5 and 6 frames back in the test), written as a
  reference list modification, and falls back to an IDR beyond the buffer. The decoder side relies on `gaps_in_frame_num_value_allowed_flag`, so a lost frame leaves a gap in
  `frame_num` that the decoder fills in instead of failing.
- **POC type 2** (no reordering) works with the driver; the POC given to VA is twice the frame number, which is what ffmpeg's own encoder passes.
- **Sizes**: the surfaces are allocated in whole macroblocks and the colour conversion places the picture at the top left (`output_region`); the SPS crops the rest. 1288x800 and
  2344x1080 decode at exactly their size. Slices (by macroblock rows) and the rolling intra refresh (in macroblock columns) work as with HEVC.
- **Levels**: the smallest H.264 level that holds the macroblock rate (`h264_level_idc`): 2304x1440 at 90 Hz is level 5.2. A device whose decoder stops at 5.1 would refuse it; not seen yet.
- Not measured: H.264 timing and bitrate against HEVC at 90 Hz, and decoding on the tablet or the Pixel (no device run yet).
