# Brightness over DDC/CI

The virtual monitor answers DDC/CI, so anything that sets a monitor's brightness can set the phone's: `ddcutil`, the brightness slider of a
desktop shell that supports external monitors (DMS lists the output as `ddc:i2c-N`), a keybinding that runs `ddcutil`.

## How it works

- evdi registers an I2C adapter ("DisplayLink I2C Adapter") for each device, and the kernel hands every transaction on address 0x37 (DDC/CI) to
  userspace through libevdi. It waits for one answer of the same length for each message, a write as well.
- `daemon/ddcci.c` is the monitor's side, with no I/O: it parses the host's request frames (checksum included) and builds the replies.
  `daemon/vdisplay.c` connects it to the libevdi callback. It supports Get VCP Feature, Set VCP Feature and Capabilities Request for one
  feature, brightness (VCP 0x10, 0 to 100, starts at 100). Everything else gets the usual "unsupported" answer or none.
- A Set makes `pdispd` send BRIGHTNESS (`docs/protocol.md`) to the phone; the app applies it to its window (`screenBrightness`), only while the
  picture shows, so the system brightness never changes. A phone that connects later is told the last level the host set (the daemon keeps it
  while it runs). The level set from the PC wins over the app's "Full brightness" switch; without one the switch applies as before.

## Using it

    ddcutil --bus N getvcp 10        # N is the bus number of the output's card, see `ddcutil detect`
    ddcutil --bus N setvcp 10 40

The bus device (`/dev/i2c-N`) belongs to the group `i2c`: add yourself (`hardware.i2c.enable = true;` and the group on NixOS), or the
slider cannot open it. `tests/m13_brightness_check.sh` skips without that access.

A brightness of 0 is applied as the lowest level the panel allows (an exact 0 switches the panel off on some devices).

## Not done

- `pdisp-ctl brightness` (the same without DDC/CI), and a slider in the tray GUI.
- Remembering the level across daemon restarts.
