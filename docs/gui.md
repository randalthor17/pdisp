# pdisp gui, the desktop application

A Qt 6 / QML application for the PC: a tray icon and a window. It talks to the daemon over the control socket (`docs/control.md`), so it can run or be closed
at any time without touching a stream.

| Page | |
|---|---|
| **Status** | Is the daemon running (with a Start button if not), who is connected, over which transport, the picture size and rate, pictures per second, bitrate, lost frames, send queue, the device's decode time, how long it has been connected, and Disconnect. While a lost device's monitor is kept, a button removes it at once. A Connections card shows whether USB and Wi-Fi are on. |
| **Devices** | The paired devices (name, key fingerprint) with Remove, and Refresh. |
| **Settings** | Highest refresh rate, starting and highest bitrate, still-picture sharpening, the video codec (Automatic, HEVC or H.264), how long a lost device's monitor is kept. Saved by the daemon (`pdispd.conf`) and used from the next connection. Port, USB and the pairing mode are daemon options, not GUI settings. |
| **Pairing dialog** | When an unknown device connects: its name, the code to compare with the one on its screen, its fingerprint, and Allow always / Allow this time / Deny. Refused after 90 s. The window comes up by itself, and a tray notification says so. |

Without a daemon connection the window says so and offers `systemctl --user start pdispd`.

## Running it
- `pdisp gui` opens the window. `pdisp gui --background` starts it in the tray only (the NixOS module does this as the user service `pdisp-gui`, after `pdispd`).
- Closing the window hides it when a tray is available (a StatusNotifier host: DankMaterialShell, KDE, waybar's tray, ...), and quits when there is none.
- With a GUI connected, new devices are approved there. With none, the daemon falls back to the prompt command (`pdisp-pair-prompt`, a notification) or the terminal.
- `--socket PATH` for a daemon with another control socket. For tests: `--screenshot FILE [--page N] [--delay MS]` writes the window to a PNG and quits (works with
  `QT_QPA_PLATFORM=offscreen`), `--test-answer always|once|deny` answers pairing requests by itself.

## Design
Material 3 Expressive, drawn by the app itself (Qt Quick Controls only supplies behaviour, in its Basic style): a navigation rail on a wide window and a bottom bar on a narrow one,
round tonal containers (20 and 28 dp), pill buttons that round off while pressed, a 52 x 32 M3 switch, segmented buttons, steppers, M3 dialogs over a scrim, and
Material Symbols Rounded icons (the system's font, else the one the Nix package bundles).

**Colours come from the Qt theme, not from any shell.** `Theme.qml` reads the Qt palette (`SystemPalette`): the window colour is the surface, the highlight colour is the
accent, and the rest of the Material 3 tonal roles (containers, the secondary and tertiary accents, outlines, surface steps, the accent at a tone that reads on the surface)
are derived from those two the way a scheme is built from a seed. Dark or light follows the window colour. Errors are red in any theme. The font is the theme's font.
So it follows qtengine, a KDE colour scheme or plain Fusion, and changes live when the theme does. `--test-palette window,text,highlight` fakes a theme for screenshots.

## Building
Optional in meson: `-Dgui=auto` (default) builds it when Qt 6 with Quick Controls, Widgets and SVG is found, `-Dgui=disabled` leaves it out. The Nix package
`pdisp-full` has the daemon and the GUI (`pdisp` is the daemon alone); `services.pdisp.gui.enable` (default on) installs and starts it. 

## Checked
- `tests/m11_gui_check.sh`: a real daemon, the GUI offscreen answering the pairing request through its own backend, a viewer as the device, screenshots of the
  status and devices pages that are not blank. And by eye in screenshots: idle, the pairing dialog (code matching the device's), a running session, the devices
  and settings pages. The packaged binary was run with the development shell's Qt variables removed, and finds its own modules.
- Not checked: the tray icon on a real session (offscreen has no tray; it needs a StatusNotifier host), the window on a real Wayland desktop, light and dark
  palettes beyond what the offscreen Fusion shows, small window sizes beyond 520x460.
- In a shell that sets `QT_QPA_PLATFORMTHEME=qtengine` the development shell's Qt loads a platform-theme plugin built against another Qt and crashes at exit
  (`KIconTheme`). The tests clear both variables; the packaged app uses the Qt it was built with.

## Tray icon
The tray shows a monochrome glyph (`gui/pdisp-tray.svg`: a monitor with a phone in front), not the coloured app icon. `main.cpp` tints it with the Qt
theme's text colour, writes it as a 64x64 PNG under `~/.cache/pdisp/pdisp-gui/` and gives that to the tray; it is redone when the light or dark scheme changes.
