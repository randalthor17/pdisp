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

## On the phone (Pixel 8a, Android 17, 2400x1080 at 60 Hz, Wi-Fi, app statistics in logcat tag `pdisp`)

The app syncs its clock with the PC (same protocol as the viewer, 100 ms apart at first, then once a second; round trip 2.5 to 3.8 ms)
and measures, per picture, PC capture time to "decoded" and to the vsync the picture was scheduled for. The scheduled time is the
display slot handed to `releaseOutputBufferAtTime`; actual scanout follows within about one vsync.

| Presentation backlog allowed | Capture to decoded p50 | Capture to vsync p50 / p95 | Pictures dropped per 300 | Late frames (> 25 ms apart) per 300 |
|---|---|---|---|---|
| 3 frames (first version) | 22 ms | 56 to 62 / 61 to 68 ms | about 0 | 0 to 5 |
| 1 frame | 21 to 23 ms | 26 to 31 / 37 to 44 ms | 14 to 24 | 9 to 13 |
| **1.5 frames (kept)** | 22 ms | 33 to 40 / 35 to 55 ms | 0 to 10 (30 to 50 in the first seconds) | 0 to 4, one window with 23 |

The backlog is a jitter buffer: the grid of display slots advances one frame per frame, so without a limit it stays as far behind as the
worst bunching it ever saw. Frame arrival (PC pacing plus Wi-Fi) has a p95 spread of 20 to 35 ms around the nominal 16.7 ms, which is what
the buffer absorbs. A smoothness/latency setting belongs in M10.

Decode in the hardware decoder: Pixel 8a (Tensor G3, `c2.exynos.hevc.decoder`) 9 to 12 ms p50 and 15 to 24 ms p95; Galaxy Tab S9 FE
(Exynos 1380, same decoder name, 3.3 MP) 48 to 60 ms p50. The decoder lists vendor parameters (`vendor.sec-dec-output.display-delay`,
`decode-order`, `dpb-num`, ...) that were not tried yet.

### Decoder vendor parameters (Pixel 8a, 2026-11-21): do not set them
`vendor.sec-dec-output.display-delay.value = 0` together with `vendor.sec-dec-output.decode-order.value = 1` made the decoder far
slower to deliver pictures: decode latency p50 **227 ms** (p95 244 ms) against **9.7 ms** (p95 16.5 ms) with the defaults, same stream,
same device, A/B by system property in one session. The defaults are already the low-latency behaviour on this decoder; the keys
were reverted. (The Tab S9 FE's 48 to 60 ms is therefore not something these two keys fix; `dpb-num`, `extra-buffer-num` and
`reorder-timestamp` remain untried and need the tablet.)


## Galaxy Tab S9 FE (SM-X510, Android 16, 1440x2304, Exynos HEVC decoder), 2026-11-22
Over USB and over Wi-Fi the tablet streams at its native size. With a steady 60 fps animated pattern it held 40 Mbit/s for 70 s with no loss
over USB and 45 s over Wi-Fi. Its decoder is slower than the Pixel's and has periodic stalls (decode p50 17-23 ms, p99 130-150 ms against the
Pixel's 16-30 ms); it starts with the real-time priority hint only (the operating-rate hint fails with "Failed to get real time resource").

**A bug found with it, not specific to the tablet:** while the tablet was being touched or written on, the bitrate fell from 40 to 2-4 Mbit/s with
no real loss, FEC rose, and the stream finally stalled. Cause: the PC numbers every datagram in one sequence, including the INPUT_ACK it sends
for each touch or pen event; the app (and the viewer) handled an ack and skipped the receiver's packet accounting, so every ack looked like a
lost packet (83% "loss" in a reproduction), and the sender's rule "above 20% loss, cut the rate" fired. Fixed (commit ae02361); a test
(`test_stream`: acks counted vs not, 0 vs 833 permille) guards the receiver's contract. After the fix, 5 minutes of use stayed at 40 Mbit/s
except one 12 s dip that matched a real 31 ms delay spike.

The daemon's stats line now also shows the worst queue delay, decode time and lowest receive rate the phone reported in the interval
(`phone: queue ... decode ... recv >= ...`). The phone never fills in `decode_ms` yet (always 0.0).

Still open on the tablet: the decoder's periodic 100+ ms stalls (not the cause of the collapse above), and a hang reported with the S Pen side
button (docs/touch.md).


### Tablet decoder stalls: the operating rate (2026-11-22)
Cause of the periodic 100-150 ms stalls on the Tab S9 FE (Exynos `c2.exynos.hevc.decoder`): not our receive loop (it collects pictures every 3-8 ms; `SLOW`
log lines say so) but the decoder itself, which held up to 4 frames in flight at 60-70 ms each. At 1440x2304 and 60 fps it ran with no headroom,
because the app asked for `operating-rate` 120, which the tablet refuses ("Failed to get real time resource"), and then dropped the rate
altogether. The operating rate is how the vendor clocks the decoder, and a rate the hardware can reserve makes a large difference
(heavy 60 fps pattern, steady state, over USB):

| operating rate | started? | decode p50 / p95 / p99 | frames > 60 ms |
|---|---|---|---|
| none (120 refused) | priority only | 19-22 / 57-69 / 70-80 ms | 60+ per minute |
| 60 | yes | 20 / 47-69 / 71-79 ms | 60+ |
| 90 | yes | 14.6 / 22 / 24-27 ms | 0 |
| 100 | yes | 13.3-15 / 17-21 / 18-27 ms | 0 |
| 110, 120 | refused | as "none" | |

Now the app tries 120, 110, 100, 90, 80, 70 and keeps the highest the decoder accepts (this tablet: 100); under real content decode is p50 15-16,
p99 27-35 ms and no slow frames. Two details that matter:
- Rates are probed with throwaway decoders that have **no display surface**, then the real decoder is created once. A start that fails with the real
  Surface attached leaves the next decoder dead (it never delivers a picture; seen after two refusals, 120 and 110, then a success at 100).
- `setprop debug.pdisp.oprate N` forces one rate (-1 = the ladder, 0 = none) for experiments.
Not checked: the Pixel with the new ladder (it accepted 120 on the first try before, so it should be unchanged).

### Reconnecting after the app is restarted over USB
The first connection after the app was force-stopped sometimes fails its handshake ("Connection timed out", or "the PC does not use the secure
protocol", probably bytes left over from the previous session in the pipe) and succeeds on the retry about 10 s later. The EPROTO case used to
end the session for good; over USB it is now retried. Why the first attempt fails is not understood yet.
