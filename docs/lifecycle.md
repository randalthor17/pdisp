# Session lifecycle (M9)

The virtual monitor belongs to the daemon, not to one connection. What happens to it depends on how the phone went away.

| What happened | Who notices, and how fast | The monitor |
|---|---|---|
| The phone disconnects on purpose (Disconnect, Back, leaving the app) | CLOSE reason 0 | removed **at once** |
| The phone's screen turns off or locks | CLOSE reason 1 | removed **at once** |
| The cable is pulled, the app is killed, Wi-Fi drops | the control connection closes (immediately) or the USB device vanishes | kept for the **grace period** (`pdispd -g SECS`, default 30), then removed |
| The phone goes silent (frozen, Wi-Fi dropped without a goodbye) | no datagram for 3 s, or no PONG for 8 s | kept for the grace period, which starts when the loss is noticed |
| The phone comes back within the grace period | | the **same monitor** is reused: nothing changes for the compositor; a fresh session (new handshake, keys and keyframe) starts on it |
| ... needing another size (it was rotated meanwhile) | | the monitor is reconfigured in place (the same hot-plug as a rotation) |
| The phone comes back after the grace period | | a new monitor, with the same identity (EDID make, model, serial) |
| The daemon stops or is restarted | | removed at once (its evdi device closes); the app keeps trying to reconnect, and a restarted daemon gets a new monitor with the same identity |
| A stranger fails the handshake, or an unauthorised device is refused | | untouched: a monitor waiting for its phone keeps waiting, with the same deadline |

Why two different treatments: a phone that is *lost* will probably be back (a few seconds of Wi-Fi trouble, the device waking from sleep),
so windows should stay where they are. A phone that *left on purpose* or *locked* is not watching, and the monitor you cannot see should
not hold windows you might need: the compositor moves them to your other monitors immediately.

What the compositor does with the windows when the monitor goes and returns is its own policy; the daemon only controls the timing and keeps
the output's identity stable. In niri, windows of a removed output move to another output and niri moves workspaces back when the
output returns. (Not yet checked here with real windows; see "Not verified" below.)

## The daemon
`pdispd` runs one loop for its whole life (`acquire_peer`, `serve`, `teardown_output` in `daemon/pdispd.c`): wait for a phone (listening on the
network, or polling for the accessory on USB, in half-second slices so the display keeps being serviced), serve it, then decide as in the
table. While the monitor waits for a lost phone the daemon keeps servicing the evdi device (so compositor updates never back up) but does
not encode. `-o` serves one session and exits, `-S` stops after a time.

## The app (`MainActivity.kt`)
- `wantConnected` and `target` (a host, or the USB cable) are what the user asked for; they outlive any one session.
- Disconnect or Back: `nativeStop(reason 0)`, no more retrying.
- The surface going away (screen off, lock, another app): `nativeStop(reason 1)` if the screen is off or the keyguard is locked, else 0; the wish
  to be connected stays, and when the screen comes back (`surfaceCreated`) it reconnects by itself.
- A session that ends by itself because the link failed is retried (after 1 s, 2 s, ... at most 5 s apart) for as long as the user wants it;
  one that ended because the PC refused it, the key is unusable or the decoder cannot do the stream is not (the message stays).

## Tested
`tests/m9_lifecycle_check.sh` (it watches only whether the monitor exists in `niri msg outputs`; no window is opened) with a 6 s grace:

| Scenario | Result |
|---|---|
| phone killed (as a pulled cable) | monitor stayed 6.16 s, then went |
| phone back after 3 s | same connector, never gone (0 gaps in 4 s of polling), 87 frames decoded afterwards |
| phone back after the grace period | a new monitor appeared |
| CLOSE reason 1 (lock) / reason 0 (on purpose) | gone 124 ms / 123 ms after the close |
| phone frozen (SIGSTOP) | noticed after 3.0 s, monitor gone after 9.2 s in all (3 s + 6 s grace) |
| phone back needing another size | the monitor followed (2400x1080), frames decoded |
| daemon restarted | monitor gone 10 ms after the daemon stopped, back with the restarted daemon under the same serial |

## Windows (the plan's scenario matrix), niri, 2026-11-22
`tests/m9_windows_check.sh` (opt-in, `PDISP_TEST_WINDOWS=1`, because it opens a window of our own test client fullscreen on the virtual monitor
and the compositor moves it) with a 6 s grace period, a scripted phone, and a real monitor `DP-1`:

| Scenario | Where the window was |
|---|---|
| phone lost, back within the grace period | stayed on the virtual monitor throughout (the monitor never left) |
| phone lost, never comes back (app closed after resume) | moved to the real monitor `DP-1` when the grace period ended |
| phone back after the monitor was removed (app open on resume) | the window went **back to the virtual monitor** on its own: niri hands a workspace back to its output |
| phone locks (CLOSE reason 1) | moved to `DP-1` at once |
| daemon restarted | moved to `DP-1` when the monitor went, and back on the virtual monitor when the restarted daemon recreated it |

So windows end up where the design says. This depends on the monitor keeping its identity (EDID make, model, serial) across removal; the
connector name (DVI-I-1) is not what niri keys on. The test restores your focus and closes its window.

## Not verified
- Other compositors (windows were checked on niri only), and a real application with several windows (the test uses one).
- Android Doze and Wi-Fi power management during a long sleep.

## On a device (Pixel 8a, Android 17, Wi-Fi), 2026-11-22
With a 15 s grace period, driving the phone with adb (nothing was locked: a locked phone cannot be unlocked from here):

| Scenario | Result |
|---|---|
| Home pressed (the app leaves the screen) | monitor gone after 646 ms ("on purpose"); returning to the app reconnected by itself and streamed |
| Wi-Fi off for 5 s | the monitor stayed; the same connector was reused when the phone came back; streaming resumed |
| Wi-Fi off for 25 s | monitor removed after 18.3 s (3 s to notice, then the 15 s grace period); the app reconnected when Wi-Fi returned and got a new monitor |
| daemon restarted under the open app | monitor gone after 222 ms; the app reconnected to the restarted daemon by itself |
| app killed | the monitor stayed for the grace period, then went; nothing reconnected |

## USB: what is verified, and an Android limit
Real pull and replug of the cable (Pixel 8a, Android 17), daemon with a 25 s grace period:
- The daemon noticed the pull in 0.9 s, kept the monitor, and after the grace period removed it (as designed).
- It then asked the phone to switch to accessory mode again, but **the phone did not switch**: after a pull Android reverts its USB mode to
  charging only, and its USB service then fails to open `/dev/usb_accessory` (`UsbDeviceManagerJNI: could not open /dev/usb_accessory`)
  until the mode is set by hand again in the USB notification. Setting it from adb (`svc usb setFunctions mtp`) did not help either. A fresh
  replug with the mode set, or force-stopping the app, did connect. So **reconnecting over USB after an unplug needs the user to re-select
  File transfer on this phone**; a program cannot do it without privileges. The Wi-Fi path reconnects by itself.
- Two bugs found on the way were fixed: the daemon asked for the accessory switch every poll slice (each request restarted the switch);
  it now waits 5 s between requests. The app never closed the accessory descriptor when a session ended; it does now.
- Still unverified: whether USB reuse of the monitor works within the grace period when the phone has been put back into File transfer in
  time. `PDISP_USB_DEBUG=1` prints the daemon's USB steps.
- Idea for later: the app can notice a cable with no accessory and tell the user to set USB to File transfer.

### What we do about it (no adb, no developer options)
Android keeps a new USB connection charging-only until the user allows data, and ignores the accessory switch meanwhile. That is its
policy for every accessory app, so the user must choose File transfer in the USB notification after (re)plugging. We make it obvious and
automatic afterwards: the app says "cable connected, but USB is charging only: open the USB notification and choose File transfer" while
it sees power over USB and no accessory, and connects by itself when the accessory appears; the daemon logs the same advice once when
a phone ignores its switch request, and keeps asking (every 5 s). Install docs must mention the step. The adb path
(`adb reverse`) is not an option: devices will not have USB debugging on.

### Re-test after the descriptor fix (2026-11-22)
Real pull and replug, grace 40 s: pull noticed at once, monitor kept; the phone came back as a plain data device and ignored the first switch
request (the daemon logged the advice); about 9 s after the replug the accessory appeared on its own (no manual File transfer choice this
time), and the daemon logged "the virtual output 1080x2400 is still there: reusing it" and streamed on it. One run; a phone that stays charging-only
still needs the user's choice.

### Repeat test (2026-11-22, no adb or USB setting changes between rounds)
Real pull and replug, 4 rounds, grace 60 s, with the fixes to the daemon (5 s between switch requests) and the app (the accessory descriptor is
closed; the blocked accessory read is interrupted with a signal, because a thread stuck inside the driver's `acc_read` kept
`/dev/usb_accessory` open and made Android's USB service log `could not open /dev/usb_accessory` and refuse every later switch):

| Round | Gap before replug | Result |
|---|---|---|
| 1 | ~5 s | reconnected by itself after 11.5 s, same monitor reused; a second session started 1.3 s later and recreated the monitor (cause not found) |
| 2 | ~5 s | reconnected after 11.7 s, same monitor reused |
| 3 | ~20 s | reconnected, same monitor reused |
| 4 | ~1-2 s | reconnected (about 1 s slower), same monitor reused |

No round needed the File transfer choice. An earlier run, before the blocked-read fix, failed every time even with File transfer chosen.

## The lock screen on the phone (2026-11-22)
Streaming over USB, the power button pressed: the app sent CLOSE reason 1 ("the phone closed the session (its screen locked)") and the
daemon removed the monitor 200 ms later, with no grace period. After unlocking, the app reconnected by itself about 11 s later (one
handshake timed out while the phone was still locked) and streamed on a new monitor.

## A second device, and a silent cable
- The daemon serves one device at a time, but it keeps accepting on its port while it streams. A device that connects then gets a plain `REJECT` with reason 3
  (busy) before any handshake and is hung up on at once (the phone shows "The PC is busy"). Before this, the connection sat in the listen queue and the app
  waited 10 s in "securing the connection".
- With `-u -W`, a USB accessory that sends nothing (the app is not running, or the user has not answered Android's prompt) is waited for up to 10 s, but the
  wait ends as soon as a network device is waiting, and that device is served next. Measured: a loopback client was served 0.45 s after it connected, with
  a silent accessory attached.
- The app shows what usually causes a slow connection after 4 s in any connecting stage ("The PC is not answering over the cable..." or "The PC is slow
  to answer..."). The space for that text is reserved, so the Cancel button does not move when it appears (it did, and taps aimed at it missed).

## GPU memory and output churn (niri)
Every time the virtual output is removed and created again, niri (as of 2026-11) can keep the GPU buffers it made for it:
[niri issue #1457](https://github.com/niri-wm/niri/issues/1457), open, no fix, reported with real monitors too (power-off, TV/desktop switching).
Seen here: a session with many output cycles left ~2,400 dmabuf handles on niri's DRM fd, nearly all sized for the 1440-row tablet output
(2304x1536 after alignment), about 24 GiB of "shared" VRAM, GTT full, while no virtual output existed. Killing Xwayland freed nothing.

What the daemon already does to avoid churn: the output exists only while a device is connected (and for the grace period after a lost
link), a returning device gets the same output back, and only a different size reconfigures it. Rotation or a size change is one
disconnect and connect of the same evdi device. Nothing is created while the daemon waits.

If GTT fills up (`cat /sys/class/drm/card1/device/mem_info_gtt_used`), log out and in: that frees it. Avoid scripts that cycle the output.
The evdi module itself can stay loaded; unloading it needs root and does not free niri's buffers.

Measured on a fresh niri session (2026-11-22, RX 9060 XT): GTT 45 MiB, niri's shared VRAM 59 MiB. Eight create/remove cycles of a 2304x1440 output
(a viewer connecting for 3 s, then a clean close) raised VRAM from 309 to 704 MiB: **about 38 MiB per cycle**, and it stayed after the daemon
exited. That is one 2304x1536 buffer set per removed output; 600+ cycles of the test suite explain the 24 GiB seen before. A real user who
connects a few times a day loses a few hundred MiB per day at most; scripts that cycle the output are the problem.

## Running the display tests without growing your session's GPU memory
`tests/niri-session.sh` (run it from a spare virtual terminal, not from the desktop): starts a throwaway niri, runs `meson test` inside it, prints the GPU memory
before and after, and quits that niri, which frees what the output cycles took. A nested niri (a window) cannot be used: it has no DRM backend and never sees evdi outputs.
Not run yet (it needs a free VT).

## Stable rate (renegotiating the refresh rate)
`refresh_cap_hz` (the GUI's "Highest refresh rate", `pdispd -R`) is a **ceiling**, not a promise. The daemon starts at the best rate the device offers
under it and watches the stream (`daemon/ratestep.c`, every 2 s while frames are being sent; a static screen proves nothing):

- a tick is *bad* if the daemon had to force a keyframe because frames stayed unresolved, if the device reported lost frames, or if its decode took 50 ms or more;
- a storm (3 or more forced keyframes in a tick, twice in a row) decides within about 4 s; otherwise three quarters of the ticks seen so far bad (at least four, so about 8 s) mean the device cannot sustain this rate: the daemon renegotiates (RECONFIGURE, the same mechanism as a rotation) to the next
  lower rate the device offers, never below 30 Hz, and starts counting again;
- 10 ticks (20 s) of healthy streaming at a lowered rate are remembered per device and per transport (its key plus USB or network: the device decides what its decoder can do, the link what it can carry) in `learned_rates` beside the settings, with the cap at the time;
  the next connection starts at that rate. A remembered rate is ignored if the cap is raised above what it was learned under, and every remembered rate is
  forgotten when the cap is set (GUI, `pdisp set refresh_cap_hz`): the user asked a new question.
- It never goes back up by itself within a session; to try a higher rate again, set the cap again.

Each step is one output reconfiguration (a hot-plug of the virtual monitor, see the memory note above), so it happens at most a few times per device, not per session.
The first case: the Tab S9 FE at 90 Hz over USB (docs/quality.md): keyframes forced every few seconds, queue 100 to 650 ms; at 60 Hz it is clean.
Tested by `tests/m12_rate_step_check.sh` (a viewer that loses 20% of the datagrams above 60 Hz: steps down, remembers, starts there, forgets).
`tests/niri-session.sh` also prints the shared VRAM of your desktop niri before and after, to show what the tests cost it. With only one evdi device it pauses
your `pdispd` and `pdisp-gui` (the tray, `pdisp gui`) services while it runs (the tests need the device), which itself removes and recreates an output if a device was streaming:
with `services.pdisp.evdiDevices = 2` the tests take the second device and nothing is paused.

## Sanitizers

The tests run with sanitizers on. `tests/sanitize.sh` builds two trees and runs every test that needs no display in each: `build-asan`
(AddressSanitizer + UndefinedBehaviorSanitizer) and `build-tsan` (ThreadSanitizer, which cannot be combined with the others). `tests/niri-session.sh`
runs the display tests, the whole suite, in both of them one after the other (a few times slower, so with a longer timeout), and counts the
sanitizer reports in each log. `PDISP_BUILD_DIR=build tests/niri-session.sh` runs the plain build instead. The memory sanitizer needs clang, and
leak checking is off (the daemon's child processes simply exit). The sanitized runs also let the GUI shut down the normal way (`PDISP_GUI_CLEAN_EXIT`),
where the normal run skips Qt's exit-time cleanup because it aborted there now and then (the cause is not found yet).
