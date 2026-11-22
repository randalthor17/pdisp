# Picture quality (M10)

## Measuring: the text page
`pdisp-pattern -X` draws a static, text-like page on the virtual output and `pdisp-view -Q` reports, every 250 ms, how close the decoded picture is to the
exact page (PSNR and mean absolute error over R, G, B). `tests/m10_text_check.sh` runs both through the real pipeline (capture, VAAPI HEVC, pdispd,
UDP, libavcodec) and fails if the final picture is below 27 dB.

The page (`tools/pattern.h`, `textpage_pixel`) is glyph-like cells 8x14 px in a 10x20 cell with anti-aliased grey edges, in four bands: black on white,
white on dark, coloured on white, and light grey on white. The glyphs are hashed noise, not letters: **denser than real text**, so these numbers are a
lower bound on what real text gets. It is not a substitute for looking at a real page.

## Static refinement
Before: after the screen stopped changing the daemon sent nothing more, so the last picture stayed as the encoder left it, which at the starting rate
for dense content is poor (**13.5 dB**, visible smearing along strokes). Now, once the screen has not changed for 150 ms, the daemon encodes the same
screen again up to 40 times (50 ms apart), each a P-frame that improves on the last: minimum QP 4 instead of 10, a budget of 50% of the normal peak
rate, starting QP 22 (`encoder_set_refine`, `daemon/pdispd.c`). A new capture stops it at once and the next change starts a new round.

| Setting (text page, loopback) | PSNR after refinement |
|---|---|
| no refinement (`-F 0`) | 13.5 dB |
| 8 frames | 19-20 dB |
| 30 frames, normal budget | 23 dB |
| 60 frames at 16 ms, normal budget | 29 dB |
| 20 frames, 400% budget | 28 dB |
| 20 frames, 800% budget | breaks the stream (queue, keyframe loop) |
| **default: 40 frames, 50 ms apart, 50% budget** | **29.8 dB** (30.4 dB at the old 200% budget) |

Options: `pdispd -F N` sets the number of refinement pictures (0 = off). The rest are environment variables while the defaults prove themselves:
`PDISP_REFINE_FRAMES`, `PDISP_REFINE_MINQP` (4), `PDISP_REFINE_QP0` (22), `PDISP_REFINE_RATE_PCT` (50), `PDISP_REFINE_GAP_MS` (50),
`PDISP_REFINE_SETTLE_MS` (150).

## Two stream bugs found on the way
Both are triggered by a very large frame (the first keyframe of this page is about a megabyte at a starting rate of 12-15 Mbit/s: over half a second on
the wire) and caused a spiral of keyframes (idr 1 to 18, a send queue of 850 ms, a picture that never arrived):
1. The sender forced another keyframe because "too many frames are unresolved", but those frames were only queued in its own send queue. A keyframe
   behind them cannot arrive sooner. `vsend_must_idr` now returns false while the send queue is over 150 ms deep.
2. The receiver gave up on any frame 100 ms after its first packet. A megabyte frame needs longer than that even though its packets keep arriving, so it
   was reported lost, which asked for another megabyte keyframe. `vrecv` now gives up 100 ms after the frame's *last* packet (or ten times that after
   its first).

## Not done yet
- Real text and the phone or tablet are still open (above); the loopback cost of refinement is measured, see below.
- 10-bit and wide gamut are blocked by evdi (see the end of this file). The refresh-rate policy and a check on real text are still open.

## Latency of a change right after refinement (loopback)
`tests/m10_burst_latency.sh`: the text page stays static and only a pixel timestamp changes every 0.4 s (`pdisp-pattern -X -T -B 0.4`), so every
change lands inside a refinement burst (40 frames x 33 ms = 1.3 s). `pdisp-view -G` counts only the first picture carrying each stamp.

| | p50 | p90 |
|---|---|---|
| `-F 0` (no refinement), 4 runs | 8.0 ms | 9 to 38 ms |
| refinement, 200% budget, 4 runs | 7.1 to 7.5 ms | 10 to 40 ms |
| refinement, 100% budget (the default), 6 runs | 9.0 to 9.8 ms | 10 to 15 ms |

Refinement costs a change nothing measurable in steady state: a new capture is encoded at once and refinement stops. Findings:
- The first two or three stamps of every run take 400 to 800 ms with or without refinement. That is the opening keyframe of this page (about
  1 MB). The test ignores the first 3 s of stamps (`pdisp-view -g 3`) and checks p99 against 150 ms: six runs gave p50 7 to 9 ms, p90 8 to 10 ms,
  p99 12 to 86 ms, worst stamp 111 ms.
- With the 200% budget, 1 run in 4 had a loss episode in the middle (1900 repair packets, 27 NACKs) that delayed three changes by 140 to 520 ms; the
  `-F 0` runs and all six runs at 100% had none. The final quality is the same at 100% (30.35 dB against 30.37 dB), so 100% is the default. If a
  real Wi-Fi link shows the same, the budget or the frame size needs a cap tied to the measured link rate.

## Over Wi-Fi (Tab S9 FE, 2304x1440, Wi-Fi to the PC, text page with a changing stamp, 40 Mbit/s target)
Loopback hid a problem: bursts of refinement frames overload a real link.

| refinement | result over 50 s |
|---|---|
| off (`-F 0`) | clean: 1 keyframe, 4 frames lost at startup, no repair storm |
| 100% budget, 33 ms apart | 1330 repair packets within 2 s, then a loss episode: queue 500 ms, keyframes 1 to 22, 38 frames lost |
| 50% budget, 50 ms apart (the default now) | clean: 1 keyframe, 10 frames lost (all at startup), 109 repair packets |
| 50% / 50 ms, a change every 1.5 s for 60 s | clean: 1 keyframe, 4 frames lost, 96 repair packets |

Loopback quality of 50%/50 ms: 29.84 dB (30.37 dB at 100%/33 ms), so the cost is small. The rate control still does not react to this kind of
loss (the target stayed at 40 Mbit/s through the episode); a budget tied to the measured link rate would be the real fix.
The picture on the tablet was not judged by eye in these runs, and the phone does not report decode time yet. One tablet, one network, a few runs each.
The NixOS firewall blocks port 47110 after every reboot until a rule is added (`docs/spikes/m5-android.md`).

## Refresh rate over Wi-Fi (Tab S9 FE, full-motion pattern, 40 Mbit/s target)
| mode | frames | repair packets / NACKs | keyframes | queue | lost |
|---|---|---|---|---|---|
| 60 Hz (default cap), 60 s | 3534 (59 fps) | 395 / 34 | 1 | about 5 ms | 0 |
| 90 Hz (`-R 120`: the tablet offers 3 rates and the best the decoder accepts is 100, so 90 is chosen), 42 s | 3655 (87 fps) | 11064 / 754 | 7 | 5 to 31 ms | 0 |

90 Hz is delivered, but Wi-Fi needs about 30 times the repairs and the receiver asked for 6 extra keyframes ("9 frames unresolved ... sending an
IDR"). Nothing was lost in the end. Whether it looks smooth was not judged by eye. 60 Hz stays the default. The phone's decode time is still not
reported, so the PC cannot tell whether the phone or the network is the limit.

## Pixel 8a over USB (accessory mode, full motion and text page)
| test | result |
|---|---|
| 60 Hz full motion, 50 s | 60 fps (3000 frames), 0 lost, 1 keyframe, queue 0 ms, 58 repair packets |
| 120 Hz (`-R 120`), before the fix | about 120 fps, but 116 keyframes in 44 s ("9 frames unresolved ... sending an IDR") with next to no loss |
| 120 Hz, after the fix | about 118 fps, 2 keyframes in 44 s. The target dips to 17 to 26 Mbit/s at times and the phone's queue reaches 40 ms: the phone is near its limit |
| text page, changes every 1.5 s, refinement on | 3 lost frames and 40 Mbit/s, or (see below) a bad start |

- **120 Hz keyframe storm.** `vsend_must_idr` assumed 60 Hz: a floor of 8 unresolved frames and a 16.7 ms frame interval. The feedback arrives at a
  fixed pace, so at 120 Hz twice as many frames are normally in flight and the floor was hit constantly. The limit now scales with the refresh rate
  (`vsend_config.frame_interval_us`).
- **Bad start with the text page.** The first keyframe of this page (about 1 MB) sometimes leaves the target at 2 to 3 Mbit/s with a queue of 0.5 to 1.3 s
  (3 of 5 runs, with refinement on or off). In one run the refinement frames at a 5.7 Mbit/s target were then lost all the time (272 frames lost
  in 44 s) and kept the rate down. Refinement now waits while the send queue is over 20 ms or the target is under 12 Mbit/s. Three runs afterwards:
  the rate recovers to 40 Mbit/s within 30 s every time (13 to 16 frames lost at startup, none later). Why the start rate falls to 2 Mbit/s over a
  cable is not understood.
- The phone's decode time still is not reported.

## Pixel 8a at 120 Hz over USB, with the phone set to 120 Hz (40 s of full motion, `-R 120`)
The panel ran its 120 Hz mode (`vsyncRate=120`, the stream's `setFrameRate` request is honoured), the PC streamed at 124 fps with about 5 keyframes
and 30 frames lost over 40 s. In the last 10 s the target fell from 40 to 6.6 Mbit/s after a burst of loss while the phone's queue was at 46 ms: at 120 Hz
the phone is at its limit and the rate control reacts to that as if it were the network. The phone reporting its decode time (not done) is what would let
the PC tell the two apart. A 60 Hz stream is the default for that reason. Also seen: after the daemon had been restarted the app did not start by itself
for the accessory (no "open pdisp?" answer), the daemon saw it and timed out every 10 s until the app was opened by hand.

## The phone reports its decode time
The app measures, per picture, the time from queuing the input buffer to the picture coming out of `MediaCodec` (it includes the decoder's own pipeline
depth) and sends the slowest of the last ~100 ms in every feedback packet (`decode_ms_x10`, `vrecv_set_decode_x10`). The daemon prints it in its stats
line ("phone: ... decode N ms", the maximum of the interval).

Pixel 8a over USB, slowest decode per 2 s: 16 to 28 ms at 60 Hz, 16 to 32 ms at 120 Hz (a 120 Hz stream has two to four frames in the decoder).
Rule (`vsend.c`, `adapt`): at 50 ms or more the phone counts as slow. A slow phone's late receive thread reads as queueing delay and what it drops reads as
loss, so the sender then ignores both for the rate cut and does not raise the rate either: it holds. Without this a phone that is behind made the rate fall
(40 to 6.6 Mbit/s at 120 Hz) although the network was fine. `tests/test_stream.c` (`test_phone_slow`): the same 80 ms queue cuts the rate to
1500 kbit/s with a decode of 10 ms and leaves it at 20000 kbit/s with 80 ms.
Not done: the PC does not lower the frame rate or the picture size when the phone is slow, and the 50 ms threshold comes from these two measurement
series only. The 6.6 Mbit/s collapse itself did not happen again in the runs after this change, so it is not shown that the rule prevents it.

## 10-bit and wide gamut: not possible on this path (decision: skipped)
- **The source is 8-bit.** The evdi 1.15.1 module only has the formats XRGB8888 and ARGB8888 (no `XR30`/`AR30` string or constant in the module), so the
  compositor's picture on the virtual output is 8-bit before it reaches the encoder. A real 10-bit or HDR stream cannot start there.
- **Wide gamut** would also need the compositor to render the output in another colour space (niri outputs are sRGB) and an EDID that claims it; evdi gives
  the compositor no way to take 10-bit anyway.
- What does work: the PC's VAAPI encoder supports HEVC Main10 (`VAProfileHEVCMain10`, encode entry point, render formats 4:2:0 8 and 10 bit). Encoding the
  8-bit source as Main10 would only reduce banding from compression in smooth gradients. A test with libx265 on a synthetic gradient (30 frames) did not
  show a useful difference (the content was too easy for the rate to matter), so that gain is unproven and was not built.
- Reopen if evdi gets a 10-bit format, or if the capture path moves away from evdi (for example a compositor-side virtual output). Then the change is: P010
  surfaces and VPP in `daemon/encoder.c`, Main10 in the VPS/SPS, the colour tags (BT.709/BT.2020, range, transfer) in the VUI, a matching EDID
  (colour characteristics, HDR static metadata block) and the phone checking `MediaCodecInfo` for Main10 and the panel's colour modes.

## Refresh-rate policy
1. **The PC picks the highest rate the phone offers that is at or below its cap `-R`, default 60.** The default stays 60 because that is the only rate that
   was clean on every link measured: tablet over Wi-Fi 60 Hz clean, 90 Hz about 30 times the repairs and 6 extra keyframes; Pixel over USB 120 Hz works
   but the phone is at its limit (see above and the decode-time rule). `-R 90` or `-R 120` is the user's call.
2. **The phone only offers rates its decoder can hold** (`limit_rates_to_decoder` in `session.c`, before HELLO): each display rate is tried as the decoder's
   operating rate with a throwaway decoder, and a rate the vendor refuses is not offered (the lowest always stays). So `-R 120` on the Tab S9 FE, whose
   decoder refuses 110 and 120, should now give 90 and never a 120 it cannot decode. This replaces the idea of clamping to the display's maximum, because
   the display and the decoder disagree on that tablet. Checked on the Pixel 8a (both rates kept, 120 chosen with `-R 120`). Not checked on the tablet: it
   is not on adb, so the new app was not installed there.
3. Not done: choosing the rate by itself from the link quality (starting at 60 and trying higher), and a per-transport default.

## Real text (loopback)
`tests/m10_real_text_check.sh` (opt-in, `PDISP_TEST_WINDOWS=1`: it opens a kitty window and moves it fullscreen to the virtual output) shows `daemon/pdispd.c`
in a terminal (font size 11, cursor blink off) at 2304x1440 and compares the viewer's decoded snapshot with the screen as the daemon captured it. The
daemon writes that reference when `PDISP_DUMP_FRAME=<file.ppm>` is set (a debug aid: the exact frame the encoder was given; the compositor's own screenshot
was not used because it also overwrites the clipboard).

| | PSNR of decoded vs captured |
|---|---|
| refinement on (default), 3 runs | 40.4, 40.4, 40.4 dB |
| refinement off (`-F 0`), 2 runs | 40.4, 37.3 dB |

Real code is far sparser than the synthetic glyph-noise page (30.4 dB there), so it already decodes well without refinement when the rate has settled;
refinement makes the result stable (the 37.3 dB run was a screen that settled while the rate was still ramping up). Looked at by eye in a crop of the
decoded snapshot: crisp, no visible ringing around glyphs. A 2304x1440 picture of text on a dark background is the only content tried; light backgrounds,
syntax-coloured themes and small fonts were not.

## Wi-Fi checks repeated on the tablet with the final build (Tab S9 FE, 2304x1440 portrait 1440x2304, Wi-Fi, 40 Mbit/s target)
Same app build as the Pixel (decode-time report, rate filter, stop fix). The adb cable was attached for control only; the stream ran over Wi-Fi.

| test | result |
|---|---|
| text page, a change every 1.5 s, refinement default (50 s) | clean: 1 keyframe, 0 lost, 0 repair packets, 40 Mbit/s throughout; phone decode 19 to 21 ms |
| full motion, 60 Hz (40 s) | 60 fps (480 frames per 8 s), 1 keyframe, 0 lost, 230 repair packets and 14 NACKs in 51 s; decode 20 to 22 ms |
| full motion, `-R 120` (40 s) | 90 Hz (the tablet's panel tops out at 90, so the three rates it offers are 90 and below); about 90 fps, 3 keyframes, 0 lost, 2472 repair packets, 225 NACKs; once the target fell to 7 Mbit/s and recovered to 40 within 8 s; decode 16 to 44 ms |

- Refinement at 50% / 50 ms is clean on this link again (3 of 3 runs in total across the two sessions), and the decode report shows the tablet at about 20 ms
  at 60 Hz, well below the 50 ms limit.
- 90 Hz over Wi-Fi still costs about ten times the repairs of 60 Hz and an occasional rate dip, but with fewer keyframes than the first measurement (3 against 7).
- The rate filter did not remove anything on this tablet (its panel stops at 90 and the decoder holds 100), so it is untested where it matters: a phone whose
  display runs faster than its decoder. The Pixel and this tablet both pass everything their panels offer.
