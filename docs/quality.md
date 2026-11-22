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
screen again up to 40 times (33 ms apart), each a P-frame that improves on the last: minimum QP 4 instead of 10, a budget of 100% of the normal peak
rate, starting QP 22 (`encoder_set_refine`, `daemon/pdispd.c`). A new capture stops it at once and the next change starts a new round.

| Setting (text page, loopback) | PSNR after refinement |
|---|---|
| no refinement (`-F 0`) | 13.5 dB |
| 8 frames | 19-20 dB |
| 30 frames, normal budget | 23 dB |
| 60 frames at 16 ms, normal budget | 29 dB |
| 20 frames, 400% budget | 28 dB |
| 20 frames, 800% budget | breaks the stream (queue, keyframe loop) |
| **default: 40 frames, 100% budget** | **30.4 dB, 12 runs out of 12** |

Options: `pdispd -F N` sets the number of refinement pictures (0 = off). The rest are environment variables while the defaults prove themselves:
`PDISP_REFINE_FRAMES`, `PDISP_REFINE_MINQP` (4), `PDISP_REFINE_QP0` (22), `PDISP_REFINE_RATE_PCT` (100), `PDISP_REFINE_GAP_MS` (33),
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
- 10-bit and wide-gamut, the refresh-rate policy, and the phone reporting its decode time to the PC are still open.

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
  1 MB). It is why the test checks p90 (default limit 50 ms), not p99.
- With the 200% budget, 1 run in 4 had a loss episode in the middle (1900 repair packets, 27 NACKs) that delayed three changes by 140 to 520 ms; the
  `-F 0` runs and all six runs at 100% had none. The final quality is the same at 100% (30.35 dB against 30.37 dB), so 100% is the default. If a
  real Wi-Fi link shows the same, the budget or the frame size needs a cap tied to the measured link rate.
