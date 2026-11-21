# M7: the monitor over a USB cable (AOA)

Result (2026-11-21, Galaxy Tab S9 FE, Android 16, USB 2.0, ADB enabled): `pdispd -u` switches the tablet to accessory mode, the app
starts on the tablet and the stream runs over the cable, 2304x1440 at 60 fps, no Wi-Fi involved and no address typed. Presented-frame
spacing in the last windows of a run: p95 16.7 ms, 0 frames over 25 ms; 6 to 10 frames lost per run, all in the first seconds
(the initial keyframe burst), none afterwards. Unplugging is noticed (`the USB cable was unplugged` / control connection closed) and the
daemon waits for the next plug-in. `tests/m7_usb_check.sh` repeats this (needs the tablet and adb for statistics only).

## How it works
- `daemon/usblink.c` (libusb): finds an Android device (any vendor, or `-V vid`), sends GET_PROTOCOL, the accessory strings and START,
  waits for it to re-enumerate as `18d1:2d00..2d05` on the same bus path, claims the accessory interface (the one with a bulk pair that
  is not ADB) and moves bytes between the bulk endpoints and two pipes. A zero-length packet follows any transfer that is an exact
  multiple of the packet size.
- `proto/usbmux.c`: link frames (`u32 length`, `u8 kind`, payload) turn the pipe into a stream socket and a datagram socket, so the daemon
  and the app's session code are the same as on Wi-Fi (docs/protocol.md section 1). Unit test under ASan and TSan.
- `pdispd -u` runs one child process per plug-in (fork) and keeps waiting after a session ends.
- App: `USB_ACCESSORY_ATTACHED` intent filter with `accessory_filter.xml` (manufacturer `pdisp`, model `pdisp`); the activity opens the
  accessory (asks for permission if it was not launched by the attach intent) and gives the descriptor to the native session.
- `contrib/udev/70-pdisp.rules` gives the logged-in user access (`uaccess`) to common Android vendors and to the accessory ids.

## Findings
- Android's `/dev/usb_accessory` returns one USB transfer per `read()` and discards what does not fit the buffer, and it cannot be
  polled or woken. The mux therefore always reads 16 KiB and reassembles frames itself, and on Android its read thread is detached on
  stop (`USBMUX_LINK_BLOCKING_READ`) because a blocked read cannot be cancelled.
- A reliable pipe still lost datagrams: a keyframe is a burst of hundreds of packets and a unix datagram socket holds far fewer. The mux
  now waits for room (up to 100 ms) instead of dropping.
- The first plug-in showed the system dialog "Open pdisp to handle pdisp second monitor?" with an "Always open" checkbox. After that
  tap, later plug-ins started the app with no dialog in the automated runs; why (a remembered choice for this build, or the debuggable
  app) is not verified. A release build on a fresh install needs checking.
- The app was flagged as not 16 KB page aligned (Android 16 shows a warning, Play requires it): the native library is now linked with
  `-Wl,-z,max-page-size=16384`.
- Switching takes under a second; from starting the daemon to the first frame took about 5 s in the test.
- The daemon waits 120 s for the app's HELLO in USB mode (a person may have to tap a prompt).

## Not verified
- ADB off (accessory ids `2d00`/`2d04`), other vendors and a USB 3 port, sleep/resume of the tablet or the PC, two tablets at once,
  and the permission prompt on a clean install.
- Latency over the cable versus Wi-Fi (the decode pipeline of about 50 ms dominates both; see the m5 notes).
