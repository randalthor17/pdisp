# USB: access, modes and reconnecting

pdisp reaches a phone over USB with Android's accessory protocol (AOA): the PC asks the phone to switch to accessory mode, the phone's app
opens the accessory, and both ends then exchange bulk transfers. No USB debugging, no developer options, no adb, no root on the phone.
(`docs/spikes/m7-usb.md` has the measurements; `docs/lifecycle.md` what happens to the virtual monitor when the cable goes.)

## 1. The PC needs a udev rule (required)
Opening a USB device needs permission. A desktop session gets it for devices its rules know. With **USB debugging on**, systemd's built-in
Android rules happened to grant it (so every early test worked), but a normal phone has debugging off and then shows up as
`18d1:2d00` (accessory only) or with no rule at all, and `pdispd -u` fails with *Permission denied*. We measured exactly that on a Pixel 8a.

`contrib/udev/70-pdisp.rules` grants the logged-in user access (`uaccess`, no group needed) to
- the accessory ids `18d1:2d00` to `2d05` (accessory, with or without adb, audio, ...), and
- the common Android vendors in their normal mode (Samsung, Google, Xiaomi, Motorola, Huawei, OnePlus, Oppo, Vivo, Sony), because the
  switch request is sent to the device before it changes mode. A vendor not on the list needs a line added.

Install: copy to `/etc/udev/rules.d/`, `udevadm control --reload`, replug. NixOS: `services.udev.extraRules`. The packaged install (M11)
must do this itself. For a test without installing: `pkexec` copy it to `/run/udev/rules.d/` (gone at reboot).

## 2. Modes a phone can be in, and what we measured
Android resets its USB mode on every unplug (to charging only by default), and the user can pick another in the USB notification.

| Phone state when the PC asks | Seen as | Result |
|---|---|---|
| charging only, USB debugging **off** (the normal case) | accessory ids only after the switch | the switch is accepted; reconnect worked by itself every time (8 s after the replug) |
| charging only, debugging **on** (`18d1:4ee7`) | adb interface visible | accepted; 4 of 4 reconnects automatic |
| file transfer / MTP (`18d1:4ee2`) | | accepted |
| already in accessory mode (`2d00`/`2d01`) | | used directly; the app must be running to open it |
| a phone that hides itself completely until the user allows data | not tested | the app says "cable connected, but USB is charging only: choose File transfer"; the daemon logs the same advice after three unanswered requests |

Not tested: other vendors, RNDIS/tethering and MIDI modes, USB hubs and docks, Android versions other than 17.

## 3. Things that go wrong, and what we found
- **The device stays open after a pull.** The app's thread blocked in the accessory driver's read kept `/dev/usb_accessory` open, and
  Android's USB service then logged `could not open /dev/usb_accessory` and ignored every later request to switch, whatever mode the user
  picked. Fixed: the app closes the descriptor and interrupts the blocked read (a signal; `proto/usbmux.c`).
- **Repeated switch requests.** The switch takes about a second; each new request restarts it, so polling every half second never
  finished. The daemon now waits 5 s between requests.
- **Leaving the app ends the session on purpose.** Opening a system screen on top of the app (the USB mode chooser) stops the app's
  surface, so the session is closed "on purpose" and the monitor is removed at once (lifecycle design). It reconnects when the app is back.
- `PDISP_USB_DEBUG=1` makes the daemon print what it does to each USB device.

## The app freezing ("pdisp isn't responding") with an unanswered USB session
A phone left in accessory mode, with a PC that listens on Wi-Fi only, connects over USB and waits 10 s for a handshake nobody answers. Typing an
address and pressing Go during that wait stopped the session on the UI thread, which joined the session thread, which sat in the handshake read (up to
10 s) or in a write to an accessory the host never reads: Android showed the ANR dialog after 5 s. Fixes:
- `nativeStop` shuts down the control socket first, which wakes a blocked handshake at once.
- `usbmux_stop` interrupts the write thread with SIGUSR2 as it already did the read thread, waits at most 0.5 s, and detaches it if it is still stuck.
- An address typed and confirmed on purpose replaces a USB session (`MainActivity`).
Not covered: `net_tcp_connect` to an unreachable Wi-Fi address also blocks the session thread, so stopping then can still wait.
