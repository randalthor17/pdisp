# Latency measurement and baseline (M4)

Two measurements, both taken by `pdisp-view` on the real pipeline (niri output, evdi, VAAPI HEVC, UDP, libavcodec).
Both are corrected for the viewer's clock using the clock sync below, so they stay valid when the viewer's clock differs
from the PC's (as a phone's does).

## What is measured

- **Capture to decode** (`latency_ms` in the `VIEW` line): from the sender's `capture_us` (the moment evdi delivered the
  frame) to the decoder finishing the frame. Everything the daemon and the network add.
- **Glass to glass** (the `GLASS` line, `pdisp-view -G` with `pdisp-pattern -T`): the pattern client draws its own commit
  time into the top 16 rows of every frame as black and white 16x16 blocks (`tools/stamp.h`: a 1010 marker and 48 bits of
  microseconds); the viewer reads them back out of the **decoded luma plane**. This starts at the application's commit, so it
  also includes the compositor's render and the wait for the next refresh, and it does not depend on any timestamp travelling
  beside the video. It stops at decode completion: display scanout and, on the phone, the MediaCodec output path are not included.

## Clock sync (`proto/timesync.c`, protocol section 3.9)

TIME_REQ/TIME_RESP once a second (every 100 ms for the first five), NTP-style: offset = ((t2 - t1) + (t3 - t4)) / 2, round
trip = (t4 - t1) - (t3 - t2), taking the sample with the lowest round trip of the last ten. Verified on the real network path:

| Case | Estimated offset | Truth | Round trip |
|---|---|---|---|
| Loopback, same clock | +1 us | 0 | 0.0 ms |
| Viewer clock skewed by 7.654321 s | -7654314 us | -7654321 us (error 7 us) | 0.0 ms |
| 8 ms each way (netem) | +2 us | 0 | 16.0 ms (measured the delay correctly) |
| 15 ms jitter, bursty loss (netem) | +5.9 to +7.1 ms | 0 | 11.8 to 14.2 ms |

With symmetric paths the offset is exact; under heavy jitter the paths are asymmetric from sample to sample and the error is
bounded by half the difference (here about 6 to 7 ms). That is fine for latency numbers, which are around 20 to 130 ms.

## Baseline (2304x1440 at 60 Hz, animated pattern, 10 s runs, software decode in the viewer)

| Condition | Capture to decode p50 / p99 | Glass to glass p50 / p99 | Longest freeze |
|---|---|---|---|
| Loopback, no impairment | 21 / 39 ms | 36.5 / 54 ms | 43 ms |
| 8 ms delay each way | 26 / 38 to 57 ms | 43 / 58 ms | 39 to 47 ms |
| Bursty ~2% loss, 15 ms jitter, 40 Mbit/s cap | 44 to 49 / 102 to 115 ms | 68 / 131 ms | 89 to 251 ms |
| Bursty ~5% loss, 15 ms jitter | 33 / 88 to 94 ms | 64 / 116 ms | 86 to 108 ms |
| 15 Mbit/s cap | 33 to 40 / 90 to 105 ms | 58 / 118 ms | 51 to 61 ms |

On a clean link the glass-to-glass figure minus capture-to-decode (about 15 ms) is the compositor: the application commits
right after a frame callback and the output is rendered at the next refresh. Decode in the viewer averages about 4 to 7 ms
in software. These are the numbers to compare the phone against at M5; the phone adds its own decode, output and scanout.

## Reconfiguration

A display change (rotation) rebuilds the virtual output, encoder and sender in a new epoch. In the real run the stream was
back with the longest interruption between 110 and 380 ms (it varies with how quickly the compositor reacts to the new EDID).
