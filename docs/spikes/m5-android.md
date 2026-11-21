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
