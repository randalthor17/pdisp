# Installing pdisp

pdisp has three parts: the **daemon** on the PC (`pdispd`, creates a virtual monitor and streams it), the **Android app** on the phone or tablet,
and a few **system and desktop settings** that let the daemon and the touch input work. On NixOS and home-manager the settings are done by the modules in
this repository. Other systems do them by hand, listed below.

Needs: a Wayland compositor that handles a new monitor (niri 26.04 is the one tried), a GPU with VAAPI HEVC encoding (only an AMD RX 9060 XT was tried), and Android 10 (API 29) or newer (tried on Android 17 on a Pixel 8a and a Galaxy Tab S9 FE).

## NixOS

Add the flake and the modules:

```nix
# flake.nix of your system
inputs.pdisp.url = "github:OWNER/pdisp";   # or a path:/ URL of a checkout
# in the nixosSystem modules:
pdisp.nixosModules.default
```

```nix
# configuration.nix
services.pdisp = {
  enable = true;
  openFirewall = true;      # only for connecting over Wi-Fi; a USB cable needs no port
  # usb = true;             # default: also look for a cable (pdispd -u -W: the network stays on too)
  # evdiDevices = 1;        # virtual displays that can be connected at once
  # extraArgs = [ "-R" "90" ];   # pdispd options, see `pdispd -h`
};
```

One rebuild does all of this:

| What | How the module does it |
|---|---|
| virtual display driver | `boot.extraModulePackages = [ evdi ]`, `boot.kernelModules = [ "evdi" ]`, and `options evdi initial_device_count=N` so a device exists at boot (adding one later needs root) |
| the user may open the phone over USB | the package's udev rule (`70-pdisp.rules`, `uaccess`: the user at the active seat gets access, no group needed) |
| Wi-Fi | with `openFirewall`, TCP and UDP 47110 are opened (all interfaces; every device must be paired and the traffic is encrypted) |
| the daemon | a systemd **user** service `pdispd`, started with the graphical session, with `-u -W -P ask -A pdisp-pair-prompt` |
| pairing prompt | `libnotify` (`notify-send`) is installed; your notification daemon must show actions (mako, dunst, GNOME, KDE and DankMaterialShell do) |

**niri: touch and pen only on the pdisp monitor.** With home-manager:

```nix
imports = [ pdisp.homeManagerModules.default ];
programs.pdisp.niri.enable = true;   # writes ~/.config/niri/pdisp.kdl
```

and add one line to your niri configuration, with the other includes: `include "pdisp.kdl"` (your niri must support `include`; the DankMaterialShell setup here already uses it) (the string is also available as `programs.pdisp.niri.include`
if you generate the config from Nix). Without the mapping the device's touches would be treated as input on whatever monitor the compositor chooses; see
`docs/touch.md`. The file contains:

```kdl
input {
    touch  { map-to-output "PNP(PDI) pdisp PDISP0001" }
    tablet { map-to-output "PNP(PDI) pdisp PDISP0001" }
}
```

`PNP(PDI) pdisp PDISP0001` is the monitor's identity (make, model, serial) with the daemon's defaults. It does not depend on the connector name.
Windows on the monitor follow it when it goes away and comes back; no window rule is needed.

`nix flake check` builds a NixOS VM with the module on (`nix/vm-test.nix`) and checks the evdi device, the udev rule, the firewall port and that the user
service starts and listens. It cannot show a stream: there is no GPU or phone in the VM.

A cable and Wi-Fi are served one device at a time, in turn. A phone that sits in USB accessory mode without the app running holds the daemon for the 10 s
handshake timeout before the next device is looked at, so a Wi-Fi device can wait up to 10 s in that case.

## Other distributions (manual)

1. **Build**: `meson setup build && meson compile -C build && meson install -C build` (needs meson, ninja, pkg-config, wayland-scanner, libevdi with its
   header, libdrm, libva, libusb-1.0, wayland, zlib, ffmpeg libraries and SDL2). This installs `pdispd`, `pdisp-keys`, `pdisp-pair-prompt`, the udev rule, the
   niri snippet (`share/pdisp/pdisp.kdl`) and a systemd user unit (`lib/systemd/user/pdispd.service`).
2. **evdi**: install the evdi kernel module for your kernel (DKMS) and load it with a device already created:
   `echo evdi > /etc/modules-load.d/evdi.conf` and `echo "options evdi initial_device_count=1" > /etc/modprobe.d/evdi.conf`.
3. **udev**: copy `70-pdisp.rules` to `/etc/udev/rules.d/`, then `udevadm control --reload`, and replug the phone.
4. **Firewall** (Wi-Fi only): allow TCP and UDP port 47110 from your LAN, for example `ufw allow from 192.168.0.0/24 to any port 47110`.
5. **Service**: `systemctl --user enable --now pdispd`. Change the options with `systemctl --user edit pdispd`.
6. **Compositor**: the niri snippet above (include it, or paste it). For other compositors map the touchscreen and the pen to the monitor, or run the daemon
   with `-H COMMAND` (`docs/touch.md`).

## The phone or tablet

Install the APK (`android/build.sh` builds `android/app/build/outputs/apk/debug/app-debug.apk`; `adb install -r` or copy it to the device).
Developer options and USB debugging are **not** needed.

- **Wi-Fi**: open the app, tap Add a PC and type its address. The first time, a notification on the PC asks whether to allow the device; the code it shows
  must match the code on the device's screen. Later, tap the PC in the list. The screens are described in `docs/app.md`. "Allow always" remembers the device (`pdisp-keys list` and `pdisp-keys remove` manage them).
- **USB**: plug the cable in. The PC switches the device into accessory mode and Android offers to open pdisp: accept. The USB mode did not have to
  be File transfer on the two devices tried (charging only worked, with USB debugging on or off); a phone that hides itself until the user allows data was not
  available. See `docs/usb.md` for the modes measured and what to do if nothing happens.

## Checking it

`systemctl --user status pdispd` shows the daemon; `journalctl --user -u pdispd -f` its log (a stats line every 2 s while streaming). `niri msg outputs`
lists the monitor `PNP(PDI) pdisp PDISP0001` while a device is connected.

## What is not covered

Other compositors beyond what `-H` can do, Intel/NVIDIA encoder differences (only an AMD RX 9060 XT was tried), non-x86 PCs, and a signed or store-distributed app.
