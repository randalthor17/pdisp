# pdisp

Use an Android phone or tablet as a second monitor on Linux, over Wi-Fi or a USB cable, with touch and pen input.

pdisp creates a real virtual monitor, so your Wayland compositor treats the phone like any other screen: windows can be dragged onto it, it has its own resolution and refresh rate, and it comes back where you left it when you reconnect.

> A personal project. It is built and tested on NixOS with the niri compositor and an AMD GPU. Other setups may work but are untested, and there are rough edges (see [Status](#status)).

## Features

- **Real extended monitor**: a virtual output made with [evdi](https://github.com/DisplayLink/evdi), with a stable identity per paired phone. Nothing compositor-specific in the capture path.
- **Low latency video**: VAAPI HEVC encoding, loss recovery (forward error correction, retransmission, reference invalidation) and a rate governor that adapts to the link.
- **Wi-Fi or USB**: the PC announces itself over mDNS, so the app finds it without typing an address. USB uses Android Open Accessory, with no adb and no developer mode. Cable and Wi-Fi fail over to each other.
- **Touch and pen**: the phone's touches and stylus appear on the PC as a `uinput` touchscreen and tablet, mapped to the pdisp monitor only.
- **Brightness from the desktop**: the phone's brightness is a DDC/CI monitor setting, so your shell's brightness slider controls it.
- **Pairing like adb**: the phone shows a code, you approve it on the PC, and traffic is encrypted with the paired keys.
- **A tray app** for status, paired devices and settings, and a `pdisp` command for everything else.

## Requirements

- Linux with a Wayland compositor that handles hot-plugged monitors (niri 26.04 is the tested one)
- The `evdi` kernel module
- A GPU with VAAPI HEVC encoding (tested on an AMD RX 9060 XT)
- Android 10 (API 29) or newer, arm64 (tested on Android 17: Pixel 8a, Galaxy Tab S9 FE)

## Install

**Phone:** download the APK from the [releases page](https://github.com/randalthor17/pdisp/releases) (or follow the repository with [Obtainium](https://github.com/ImranR98/Obtainium)).

**PC, NixOS:** add the flake and enable the module, which sets up evdi, the udev rule, the firewall port, the daemon and the tray app:

```nix
inputs.pdisp.url = "github:randalthor17/pdisp";
# modules: pdisp.nixosModules.default
services.pdisp.enable = true;
services.pdisp.openFirewall = true;   # Wi-Fi only; a USB cable needs no port
```

**PC, other distributions:** build with meson and set up evdi, udev, the firewall and the service by hand. The full steps, and the niri touch mapping, are in [docs/install.md](docs/install.md).

## Usage

1. Start the daemon (the NixOS module starts it with your session): `systemctl --user enable --now pdispd`.
2. Open the app on the phone and plug in the cable, or pick the PC under **Found on your network** (found automatically over mDNS; **Add a PC** takes an address if it is not found).
3. First time only: compare the code on the phone with the one in the PC's notification and approve it.
4. A new monitor appears. Arrange it in your compositor's settings and use it.

The `pdisp` command:

```
pdisp status | watch | devices | remove | disconnect | pairing | answer | set
pdisp brightness [N | +N | -N]
pdisp keys list | remove | identity
pdisp gui [--background]
```

Details are in [docs/control.md](docs/control.md).

## Documentation

| | |
|---|---|
| [docs/install.md](docs/install.md) | installing on NixOS and elsewhere, compositor setup |
| [docs/app.md](docs/app.md) | the Android app |
| [docs/gui.md](docs/gui.md) | the tray app |
| [docs/control.md](docs/control.md) | the control socket and `pdisp` commands |
| [docs/security.md](docs/security.md) | pairing and encryption |
| [docs/usb.md](docs/usb.md), [docs/discovery.md](docs/discovery.md) | USB, mDNS discovery and failover |
| [docs/touch.md](docs/touch.md) | touch and pen input |
| [docs/brightness.md](docs/brightness.md) | brightness over DDC/CI |
| [docs/protocol.md](docs/protocol.md) | the wire protocol |
| [docs/latency.md](docs/latency.md), [docs/quality.md](docs/quality.md) | measurements |
| [PLAN.md](PLAN.md) | design and plan |

## How it works

```
 PC                                                         Phone
 virtual output (evdi) -> capture -> HEVC encode (VAAPI) -> network / USB -> decode -> screen
 uinput touchscreen    <----------------------------------- touch and pen events
```

`pdispd` is the daemon. `docs/protocol.md` describes the messages, and `PLAN.md` the reasoning behind the design.

## Building and testing

```
nix develop
meson setup build && meson compile -C build
meson test -C build
```

The tests that create virtual outputs must not run in your live session. Run `tests/niri-session.sh` from a spare virtual terminal (Ctrl+Alt+F3): it starts a throwaway niri and runs the whole suite under the AddressSanitizer, UBSan and ThreadSanitizer builds ([docs/lifecycle.md](docs/lifecycle.md)).

The Android app builds with Gradle from `android/` (CI builds and signs the release APK).

## Status

Version 0.3. It works daily for the author. Known gaps: no light-theme, TalkBack or large-font pass in the app, one device at a time, and each created output costs a niri session some GPU memory until it exits ([niri #1457](https://github.com/YaLTeR/niri/issues/1457)).

## License

[MIT](LICENSE), copyright S. M. A Kahar Auhon. The vendored Monocypher in `third_party/` is BSD-2-Clause or CC0 ([licence](third_party/monocypher/LICENCE.md)).
