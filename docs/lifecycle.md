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

## Not verified
- **Windows.** The pass check in the plan is about where windows end up. The monitor's existence and timing are tested, but not niri's
  handling of real windows on it (unplug with windows open, resume with the app open or closed). That needs windows opened on the virtual
  output, which moves them onto your real monitor's workspaces when it goes, so it waits for your OK.
- **The app on a device**: the lock/unlock, background/foreground and reconnect logic builds but was not run on a phone yet (the test phone
  was locked), nor USB unplug and re-plug.
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
