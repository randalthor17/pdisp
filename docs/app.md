# The Android app

Jetpack Compose with Material 3 (colours follow the wallpaper on Android 12 and newer, light and dark follow the system). The video is a `SurfaceView`
under the Compose screens; the native session (`android/app/src/main/cpp/session.c`) is unchanged apart from a structured status (`nativeInfo`: stage,
pairing code, live numbers).

## Screens
| Screen | What it does |
|---|---|
| Start | A USB card (what to do, or the USB state: waiting for the cable, charging-only advice, waiting for permission), the saved PCs (tap to connect; the menu renames or removes one), **Add a PC** (address, optional name; the keyboard opens only in that dialog), a "No PC yet" guide with three steps, and the last problem in a card with what to try (`explain()` in `Session.kt` turns the native text into a title and a hint). Settings are behind the gear. |
| Connecting | Progress and what is happening (connecting, securing, waiting for the PC's monitor, starting the video, or "lost the PC, reconnecting (attempt N)"), Cancel. |
| Pairing | The code to compare with the PC's notification, in large type. Cancel. |
| Streaming | The picture, edge to edge, system bars hidden. Every touch and pen event goes to the PC. **Back** opens a small menu (connection, picture size and rate, statistics switch, Disconnect, Close); Back again closes it. An optional statistics line sits in a corner and lets touches through. |
| Settings | Keep the screen on, palm rejection, statistics, the highest refresh rate this device offers the PC (Auto or one of the display's rates), forget this device's pairing (deletes its key and trusted PCs), about. |

## Behaviour kept from before
Connecting survives a lost link (reconnect with backoff up to 5 s); leaving the app or locking the screen tells the PC why; an address chosen on purpose replaces
a USB session that nobody answers; stopping a session never blocks the UI thread (`docs/usb.md`); USB attach starts the app and connects over the cable;
rotation is reported to the PC.

## Checked on a Galaxy Tab S9 FE (Android 17)
Start, connecting, pairing (the code matched the PC's), streaming over Wi-Fi and over USB, the Back menu, Disconnect, the settings screen, the Add dialog,
rotation through `tests/m5_rotate_check.sh` (62 fps afterwards), touch reaching the virtual monitor at the exact position. The last-used label updates.

## Not done or not checked
- **The phone layout** (Pixel 8a) with this UI: no phone was attached while it was written. The screens are one centred column up to 640 dp, so they should fit.
- The light theme, TalkBack and large font sizes were not looked at.
- No automatic discovery of PCs: addresses are typed. (mDNS on the PC and in the app would remove that step.)
- While streaming there is no way to open the menu except Back (a gesture-navigation back swipe works too); a three-finger gesture or an edge handle was not added
  because the PC's own gestures use touches.
- On a tablet with a floating keyboard the keyboard can cover the Add dialog's Connect button; the keyboard's own Go key still works.
