# M5: Android app on the Galaxy Tab S9 FE (SM-X510, Android 16, Exynos 1380)

Result (2026-11-21): the tablet shows the PC's virtual monitor at its native 2304x1440, 60 fps over Wi-Fi (PC wired, tablet on
the same 2.4/5 GHz LAN), colours correct (BT.709 tags honoured by the decoder), 2 frames lost in the first 30 s and repaired.
First 30 s of the animated test pattern: 357 to 477 frames sent, 40 Mbit/s target, FEC 6 to 8%, 27 NACKs answered.

## Findings
- `c2.exynos.hevc.decoder` refuses to start when asked for real-time priority and an operating rate ("Failed to get real time
  resource", error 1100). The app tries with the hints, then retries without them; the retry works and low-latency mode is kept.
- The NixOS firewall dropped the tablet's TCP and UDP to port 47110 (ping worked). The test used a temporary rule limited to
  the LAN subnet. A permanent setup needs `networking.firewall.allowed{TCP,UDP}Ports = [ 47110 ];` in the system config
  (decide with the packaging work).
- The Android SDK from nixpkgs is read-only: pin `buildToolsVersion` to the installed one and point AGP at the SDK's `aapt2`
  (`android/build.sh` does both).
- Reported size is the display's real pixels rounded down to a multiple of 8. This tablet is 1440x2304, so no cropping.

## Not done yet
- Rotation (the app is locked to landscape), pairing, touch, a latency measurement on the phone (needs the clock-sync and stamp
  reading in the app), hardware decode time, and a long Wi-Fi run with congestion.

## Stutter investigation (same day)
First on-device run stuttered. Instrumented the app (CADENCE = spacing of presented pictures, ARRIVAL = spacing of complete frames
reaching the decoder, DECODE = time in the decoder; all in logcat tag `pdisp`) and ran the animated stamped pattern for 40 s.

| Change | Presented spacing p95 / p99, frames over 25 ms per ~300 |
|---|---|
| Before (render immediately, drop older picture when two are ready, panel at 90 Hz) | 35 to 55 ms / 65 to 75 ms, 25 to 60 |
| Wait up to 6 ms for a decoder input buffer instead of refusing (refusals 27 per 30 s to 0 to 2), 8 MiB UDP receive buffer | unchanged spacing, but frame loss fell from 78 to 0 to 7 per minute |
| `releaseOutputBufferAtTime` on a one-frame-interval grid, no dropping unless 3+ frames behind, `preferredRefreshRate` 60 so the panel runs at 60 Hz | 16.7 to 20 ms / 24 to 59 ms, 2 to 10 |

Findings:
- Network delivery was fine once the receive buffer and input-buffer wait were fixed; frame arrival p95 is 23 to 37 ms (Wi-Fi jitter).
- The Exynos HEVC decoder holds 20 to 60 ms of pictures (DECODE p50), p95 about 70 ms. It accepts the priority hint but not the
  operating-rate hint. This is latency, not stutter, but it is the next thing to look at (resolution, fewer frames in flight, H.264
  comparison).
- Samsung runs the panel at 90 Hz unless asked; `ANativeWindow_setFrameRate` did not change it, the window's `preferredRefreshRate` does.
- Remaining: a few per cent of frames still arrive more than 25 ms apart (Wi-Fi jitter); `refused` still reaches 10 per 30 s late
  in a run, each costing a keyframe.
