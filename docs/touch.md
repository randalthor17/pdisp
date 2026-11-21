# Touch (M8)

The phone's touches become a virtual multitouch touchscreen on the PC. The compositor does the rest (focus, gestures, scrolling);
neither the app nor the daemon interprets gestures.

## Path
phone `MotionEvent` -> `nativeTouch` -> `input_tx` queue -> INPUT datagram (encrypted like everything else) -> `pdispd` ->
`input_rx` (order, de-duplication) -> `daemon/touch.c` (`/dev/uinput`, protocol-B multitouch, `INPUT_PROP_DIRECT`) -> compositor.

- **Reliable without waiting**: every INPUT packet repeats all events the PC has not acknowledged (`proto/inputseq.c`, spec section
  3.8), so a lost packet is covered by the next one. Verified with half of the phone's INPUT datagrams dropped: every event applied
  once and in order. If the queue ever fills, superseded moves go first; only if a down or up must be dropped does the packet carry a
  loss marker and the PC lifts every finger, so no finger is stuck down.
- **Coordinates**: the phone sends fractions of its display (0..65535); the PC maps them onto the virtual output's pixel range, so a
  touch at the same place on the phone lands on the same place of the monitor. Tested: a tap at phone pixel (540, 1200) is
  (539, 1199) on the PC (0-based), end to end on a Pixel 8a.
- **Capabilities**: the device is created when a secure session starts if the phone's HELLO says it has touch (`max_touch_points`
  slots, at most 10), with the phone's resolution and density, and is rebuilt when a rotation changes the size. It is not created
  with `-I` (insecure) or `-T`.
- **Not done**: stylus (pressure and tilt are sent but the PC creates only a touchscreen), hover, mouse-style cursor. The tool type
  is carried for later.

## Binding the touchscreen to the virtual output
A compositor has to know which output a touchscreen belongs to; there is no standard protocol for a client to say so. Without it, niri
sends the touches to its first output (we measured: the taps went to the real monitor).

**niri** (one line, once, in `~/.config/niri/config.kdl`, inside the existing `input { }` block):

    input {
        touch {
            map-to-output "PNP(PDI) pdisp PDISP0001"
        }
    }

The string is the virtual output's identity as niri names it: `PNP(<EDID manufacturer>) <monitor name> <serial text>`, i.e.
`PNP(PDI) pdisp PDISP0001` with the daemon's defaults (`-n`, `-t` change it). It does not depend on the connector name (DVI-I-1,
DVI-I-2, ...) that evdi assigns. The daemon prints the exact line when it creates the touchscreen. Notes:
- niri's `touch { }` section applies to **all** touchscreens, so a PC that also has a physical touchscreen cannot use this.
- niri applies the config live; no restart.

**Other compositors**: `pdispd -H COMMAND` runs a shell command when the touchscreen exists, with `PDISP_OUTPUT` (the connector name,
read from sysfs), `PDISP_OUTPUT_ID` (the identity above), `PDISP_TOUCH_DEVICE` (the device name), `PDISP_WIDTH` and `PDISP_HEIGHT`.
For sway that is `swaymsg input "$(...identifier...)" map_to_output "$PDISP_OUTPUT"`; GNOME and KDE keep the mapping in their own settings.
These hooks are not tested here.

## Tests
- `tests/test_inputseq.c`: the event stream under loss, duplication, reordering and outages.
- `tests/test_touch.c`: the uinput device's events as the kernel emits them. The node is root-only, so it skips for normal users;
  `pkexec build/test_touch` runs it for real.
- `tests/m8_touch_check.sh`: a scripted phone (tap, swipe, two-finger pinch) through the real daemon, landing in a Wayland window on
  the virtual output at the exact pixels, with and without 50% loss of the phone's INPUT datagrams. **It changes the running niri's
  config for a few seconds, so it only runs with `PDISP_TEST_NIRI_CONFIG=1`.**
- `tools/pdisp-touch-demo`, `tools/pdisp-evdump` (root): for experiments without a phone and without touching the compositor.
- On a phone: `adb shell input tap/swipe` into the app, and `pkexec build/pdisp-evdump` on the PC (used for the Pixel 8a result above).

## Known limits
- **Touch must stay on the virtual display (requirement, user 2026-11-22).** Without the binding above niri sends the touches to its
  first output, i.e. onto a real monitor. So the binding is not optional: the daemon should say loudly when it has not been confirmed
  (the line it prints), and packaging (M11) should ship it as an include for niri and the hook for other compositors. A compositor-free
  fallback (scaling the touch range to the virtual output's rectangle inside the whole layout) is possible but fragile, and is not done.
- Compositor gestures (scroll, pinch) are the compositor's: the events are standard touchscreen events, but whether niri 26.04 or
  an application turns them into scrolling or zooming was not checked here.
- A plugged-in phone's app prefers the USB path when the cable is attached, even when the PC is only listening on Wi-Fi; it then
  needs a second Connect. (Session lifecycle, M9.)

## Stylus
Fingers become the touchscreen; a pen becomes a second device, a **pen tablet** (`pdisp stylus`: `BTN_TOOL_PEN`/`BTN_TOOL_RUBBER`, tip
pressure 0..4095, hover distance, tilt in degrees, the two barrel buttons). It exists when the phone's HELLO says it has a stylus
(the app checks for a pen digitiser). Event meaning for the pen (spec section 3.8): hover = near but not touching (`pressure` is the
distance), down/move/up = the tip, cancel = the pen left proximity, every event carries the button state, and a pen turned over to its
eraser end leaves one tool and enters the other. Tested without privileges by recording the device's events (`tests/test_inputdev.c`) and,
as root, on the real device: the system classifies it as a tablet (`ID_INPUT_TABLET=1`) and a scripted pen stroke through the
secure daemon comes out with the right positions, rising pressure, tilt, the held button, the eraser and the pencil hover
(`pkexec build/pdisp-evdump -n "pdisp stylus"`).

**Mapping the pen**: niri has a separate section, `input { tablet { map-to-output "PNP(PDI) pdisp PDISP0001" } }` (valid in niri
26.04; the daemon prints both lines). **Not verified with a real pen**: the Pixel 8a has none and `adb shell input` cannot inject a
stylus tool type. The Kotlin code that reads tool type, hover, barrel buttons and tilt (the tilt/orientation to x/y-tilt conversion in
particular) needs a run on the Galaxy Tab S9 FE with its S Pen.

## Palm rejection (`proto/palm.c`, applied in the app before anything is sent)
1. **The pen wins**: when the pen comes near, every finger already down is cancelled (it is the hand holding the pen), and new fingers are
   refused while the pen is near and for 400 ms after it leaves (the pen counts as gone after 600 ms of silence in case its leave was missed).
2. A contact Android calls a palm (`TOOL_TYPE_PALM`) or has cancelled (`FLAG_CANCELED`) is refused, or cancelled if it was already sent.
3. A contact whose major axis is longer than about 25 mm is a palm, and so is a finger that grows past that while it moves.
A refused contact stays refused until it lifts. It is on by default; `PdispNative.nativeSetPalmRejection` switches it off (no settings
screen yet). Tested exhaustively as logic (`tests/test_palm.c`); the real contact-size values depend on the device reporting
`getTouchMajor`, which is not checked on a tablet.

## Gestures: verified
Pinch-zoom, zoom-out and drag-pan work in a real application: `loupe` (GTK4) fullscreen on the virtual output, driven by a scripted
phone through the secure daemon. Window screenshots after each gesture show the picture zoomed in about 4x after the pinch, panned
down after the vertical drag, and back to fit after the reverse pinch. (`tests/m8_gesture_check.sh`; it changes the running niri's config
for about 30 seconds, so it needs `PDISP_TEST_NIRI_CONFIG=1`.) This is the toolkit's gesture recognition working on our multitouch
stream. What niri itself does with touchscreen gestures (for example a three-finger swipe) was **not** tested, because it would move your
workspaces; the events are standard touchscreen events, so whatever niri implements for touchscreens applies.
