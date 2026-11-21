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
screen again up to 40 times (33 ms apart), each a P-frame that improves on the last: minimum QP 4 instead of 10, a budget of 200% of the normal peak
rate, starting QP 22 (`encoder_set_refine`, `daemon/pdispd.c`). A new capture stops it at once and the next change starts a new round.

| Setting (text page, loopback) | PSNR after refinement |
|---|---|
| no refinement (`-F 0`) | 13.5 dB |
| 8 frames | 19-20 dB |
| 30 frames, normal budget | 23 dB |
| 60 frames at 16 ms, normal budget | 29 dB |
| 20 frames, 400% budget | 28 dB |
| 20 frames, 800% budget | breaks the stream (queue, keyframe loop) |
| **default: 40 frames, 200% budget** | **30.4 dB, 12 runs out of 12** |

Options: `pdispd -F N` sets the number of refinement pictures (0 = off). The rest are environment variables while the defaults prove themselves:
`PDISP_REFINE_FRAMES`, `PDISP_REFINE_MINQP` (4), `PDISP_REFINE_QP0` (22), `PDISP_REFINE_RATE_PCT` (200), `PDISP_REFINE_GAP_MS` (33),
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
- Refinement was measured on the synthetic page and over loopback only. Not measured on the phone or the tablet, and not on real text.
- The latency cost of refinement: a capture that arrives while a refinement frame is still being sent is skipped until the queue drains (up to about
  one refinement frame's transmission time). The existing latency tests do not trigger refinement (the screen never settles there).
- 10-bit and wide-gamut, the refresh-rate policy, and the phone reporting its decode time to the PC are still open.
