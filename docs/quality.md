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
