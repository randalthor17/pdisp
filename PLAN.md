# Android as Linux Second Monitor: Project Plan

Working name: `pdisp` (placeholder, rename freely).

Goal: use an Android phone or tablet as a real extended monitor on Linux, with touch input, over USB (no adb setup) or Wi-Fi. The core must not depend on any one compositor. Niri is the reference target; other compositors come later.

## Working conventions

- Track everything with git. `user.name` is "claude", `user.email` is "claude@anthropic.com".
- Work in small steps. Before each step, explain the reasoning and the alternatives. The author reviews, then the step is implemented.
- Every commit is tested and validated before it is made. A commit that cannot be tested yet says so in its message and states what will test it.
- Every release-tag-worthy commit (0.1, 0.2, ...) gets a tag, and a git bundle is handed to the author: `git bundle create pdisp-<version>.bundle --all`.
- Findings from feasibility spikes go in `docs/spikes/` as short notes, with the exact commands and kernel versions used.
- Anything unverified in this plan is marked "unverified". Turn each into a verified fact or a design change during M0.

## Architecture

Pipeline: virtual output (kernel) -> capture -> encode -> transport -> Android decode and display. Touch travels the reverse way.

1. **Virtual output**: out-of-tree `evdi` (packaged in nixpkgs), driven through `libevdi`. The daemon creates a display with a generated EDID (`evdi_connect`) and removes it (`evdi_disconnect`) for hotplug. Compositors see an ordinary monitor, so no compositor code is needed here. The EDID make, model and serial are stable per paired phone, so compositors can remember the output between sessions. The connector name (`DVI-I-n`) is not stable, so identity must come from the EDID. In-tree `vkms` was rejected in M0: no EDID control through kernel 7.0, and no configfs before 6.19 (see `docs/spikes/m0-virtual-output.md`).
2. **Capture**: `libevdi` delivers frames and one bounding dirty rect per update directly to the daemon (`evdi_request_update`, `evdi_grab_pixels`). No scanout capture, no `CAP_SYS_ADMIN` in the capture path. Updates only arrive when the screen changes. Keep requesting while an update is ready and block only when none is.
3. **Encode**: VAAPI, HEVC 8-bit 4:2:0. Low-latency settings, no B-frames, CBR with a small VBV buffer to keep keyframes near P-frame size. No periodic IDR: send one on connect and on request, SPS/PPS with each. Intra refresh is advertised by the driver but untested, and needs direct libva. A high-quality refresh frame is sent when the screen goes static. 10-bit, wide-gamut and AV1 come later. Colour range and matrix tags must be set deliberately.
4. **Transport**: a small interface with two implementations. TCP is for development and Wi-Fi. Android Open Accessory (AOA) over USB is the zero-setup wired path (libusb switches the phone to accessory mode, the app launches on plug-in, one permission tap).
5. **Protocol**: versioned framing with a handshake and then video, input, cursor and heartbeat messages. The phone reports resolution, density (for physical size in the EDID), orientation, safe-area insets, supported refresh rates, supported codecs, color capabilities (wide gamut, HDR types), and touch and stylus capability. The PC replies with the chosen mode.
6. **Android app** (Kotlin): `MediaCodec` surface decode with low-latency hints, fullscreen immersive, keep-screen-on, Wi-Fi low-latency lock. It sends raw touch as normalized coordinates, pointer IDs and pressure. The cursor arrives as its own message and is drawn on the phone.
7. **Input on the PC**: a `uinput` multitouch touchscreen (`INPUT_PROP_DIRECT`). The compositor handles gestures; neither the app nor the daemon does. A small touch-binding layer maps the device to the virtual output.
8. **Security** (modelled on adb): the phone generates a keypair. The daemon keeps an authorized-keys file. The first connection shows an approval prompt on the PC with an "always allow" option. Wi-Fi traffic is encrypted using the same keys.
9. **Session lifecycle**:
   - Unplug or link loss: wait out a grace period, then disconnect the connector. The compositor moves windows to the main monitor.
   - Reconnect within the grace period (including after resume from sleep, if the app is open): the same connector comes back with the same identity, and the compositor restores windows where it supports that.
   - App not open after resume: grace period expires, connector disconnects, windows go to the main monitor.
   - Screen lock ends the session cleanly.

## Compositor-specific parts (kept small and optional)

- Touch-to-output binding. Order of attempts: generic hint (`WL_OUTPUT` udev property, unverified beyond Weston), libei through the RemoteDesktop portal (unverified for niri), then a per-compositor adapter. Niri's `map-to-output` touch setting is the first adapter (unverified). If everything fails, the display still works and touch is unmapped, with a warning.
- Window migration on unplug and restore on reconnect is compositor policy. We control only timing and output identity. Test it per compositor.

## Milestones

Each milestone has a pass check. Do not start the next one until the check passes.

**M0: Feasibility spikes** (throwaway code, results in `docs/spikes/`). Done 2026-11-21.
- vkms per-connector EDID and hotplug control: **no** (no configfs on 6.18; no EDID attribute upstream through 7.0). Design change: use evdi.
- evdi: EDID identity, hotplug, frames and dirty rects: **yes**, on 6.18.63.
- Niri accepts the virtual output as a normal monitor: **yes** (vkms and evdi).
- Capture: **yes**, through `libevdi`, 1 to 2 ms per frame. The `drmModeGetFB2` path is no longer needed.
- VAAPI HEVC low-latency encode on the RX 9060 XT: **yes**, about 2 ms per frame. 2304x1440@90 sustained live (88.8 fps).
- AOA on the target tablet (Samsung, Android 16): **yes** (protocol 2, accessory mode in ~800 ms, USB 2.0 high speed).
- Open, carried forward: intra refresh in practice, raw-capture text quality, tablet `MediaCodec` accepting the stream (M5), AOA permission prompt and auto-launch (M7).

**M1: Virtual output** (done 2026-11-21; `meson test` runs the checks, the niri one needs a free evdi device)
- EDID generator (timing, physical size, name, serial, later chromaticity) with unit tests checked against `edid-decode`. Physical size must be right in the base-block centimetre fields as well as the timing descriptor, because niri reads the former.
- Daemon creates an evdi device, connects it with the EDID, and disconnects and reconnects it on demand.
- Pass: `niri msg outputs` shows the output with the right mode and physical size. Disconnect and reconnect keeps the same output identity (make, model, serial).

**M2: Capture** (done 2026-11-21: bit-exact pattern round trip; animated capture ~58 fps at 2304x1440@60 with 1.2 ms average and 5.7 ms worst grab, mostly partial rects; see `tests/m2_capture_check.sh`)
- Receive frames and dirty rects from `libevdi`, with the request-while-ready loop.
- Pass: a test pattern on the virtual output round-trips to a PNG that matches. Capture time per frame is logged. A moving window produces partial rects and no more than one update per compositor frame.

**M3: Encode**
- VAAPI HEVC low-latency from captured frames, output to a file. Prefer direct libva in the daemon; ffmpeg is acceptable for the first version.
- Pass: the file plays in `mpv` and `ffprobe` reports the expected stream. Encode time per frame is logged. The keyframe size stays near the largest P-frame size under the chosen CBR settings.

**M4: Protocol, TCP transport, desktop test client, latency harness**
- Framing, handshake, video packets, heartbeat. Protocol fuzz and round-trip tests.
- A desktop test client that decodes and shows the stream.
- A latency harness that stamps frames and measures glass-to-glass.
- Pass: live view on the desktop. Latency numbers recorded for the baseline.

**M5: Android app skeleton**
- Kotlin app: TCP connect, handshake with real resolution, density and refresh rates, `MediaCodec` surface decode.
- Pass: the phone shows the virtual monitor at its native resolution.

**M6: Pairing and security**
- Keypair, authorized-keys store, approval prompt, encrypted transport.
- Pass: an unauthorized key is rejected. An authorized key reconnects without a prompt. Tests cover both.

**M7: AOA transport**
- libusb switch to accessory mode, bulk endpoints, udev rule, app launches on plug-in.
- Pass: plug in, tap permission once, and the monitor appears with no adb or developer mode.

**M8: Touch**
- `uinput` multitouch device created from handshake capabilities. Touch messages over the protocol. Niri binding adapter.
- Pass: touches land on the phone's output, and compositor gestures (scroll, pinch) work.

**M9: Session lifecycle**
- Grace period, hot-unplug, sleep and resume, lock.
- Pass: a scenario matrix on niri: unplug with windows open, resume with app open, resume with app closed, lock, daemon restart. Windows end up where the design says.

**M10: Quality and rates**
- Refresh rate mode selection, adaptive bitrate, static refinement, optional 10-bit and wide-gamut with matching EDID and stream color tags.
- Pass: visual check on text-heavy content. Latency under load stays within the recorded baseline plus a margin chosen at M4.

**M11: Packaging and 0.1 release**
- NixOS flake and module: evdi (`boot.extraModulePackages`, module options, device creation), udev rules, systemd user service. Install docs.
- Pass: a clean NixOS configuration reaches a working session from one rebuild. Tag 0.1 and hand over the bundle.

Later: other compositors (sway, KDE, GNOME), audio, clipboard, multiple phones.

## Repository layout (proposed)

- `daemon/`: virtual output, capture, encode, transport, session.
- `proto/`: shared wire format definitions and tests.
- `android/`: Kotlin app.
- `tools/`: desktop test client, latency harness, EDID tools.
- `docs/`: design notes, `docs/spikes/`.
- `nix/`: flake and module.

## Open decisions

- Daemon language. The leaning is C: `libevdi`, VAAPI and libusb are all C APIs, the spike code was plain C with little glue, and it matches the author's systems background. Rust and Zig are possible. Confirm at the start of M1.
- Share the protocol library between daemon and app as C (via JNI) or write it twice with a spec and tests. Decide at M4.
- Project name.
- Privilege model. Capture no longer needs `CAP_SYS_ADMIN`. The remaining privileged steps are loading the evdi module and adding a device (`/sys/devices/evdi/add`), so a user service plus a udev rule or a one-shot privileged setup unit (module options, device count) is the likely answer. Decide at M1.

## Risks

- evdi is out-of-tree. It must be rebuilt for each kernel (nixpkgs does this), and DisplayLink only claims verification up to 6.15. It worked on 6.18.63; newer kernels are unverified. If it breaks, the fallbacks are vkms plus hotplug on 6.19+ without EDID identity, or forcing a spare physical connector with `drm.edid_firmware`.
- evdi dirty rects are one bounding box per update, and full-screen when the whole output animates. They are a hint, not a precise region list.
- Compositor-to-evdi copy cost was not isolated. It kept up at 2304x1440@90 in M0, but heavier scenes and 4K are unmeasured.
- The tablet is on USB 2.0 high speed. That is enough for the measured HEVC stream (about 20 to 40 Mbit/s), but limits headroom for high bitrates, static refinement and 10-bit.
- Keyframes on desktop content are about 7 times a P-frame unless rate control is constrained. Intra refresh is advertised but untested.
- AOA behavior varies by phone vendor firmware. The permission prompt and auto-launch on Android 16 are untested until the app exists.
- Touch-to-output binding may fail on compositors without a binding mechanism.
- Compositors may not restore windows on output reconnect.
