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

1. **Virtual output**: in-tree `vkms` with the configfs interface, creating a connector with a generated EDID. Compositors see an ordinary monitor, so no compositor code is needed here. Connector name and EDID serial are stable per paired phone, so compositors can remember the output between sessions.
2. **Capture**: a privileged daemon reads the CRTC scanout buffer (`drmModeGetFB2` plus PRIME export, needs `CAP_SYS_ADMIN`). Fallback: PipeWire capture of that one output. The compositor still copies into vkms buffers, so measure that copy.
3. **Encode**: VAAPI, HEVC 8-bit 4:2:0 at a high bitrate first. Low-latency settings, no B-frames, intra refresh, SPS/PPS on every keyframe. A high-quality refresh frame is sent when the screen goes static. 10-bit, wide-gamut and AV1 come later.
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

**M0: Feasibility spikes** (throwaway code, results in `docs/spikes/`)
- vkms: kernel version, `CONFIG_DRM_VKMS`, configfs present, per-connector EDID and hotplug control.
- Niri accepts the vkms output as a normal monitor.
- Scanout capture via `drmModeGetFB2` works with `CAP_SYS_ADMIN`, and its cost.
- VAAPI HEVC low-latency encode works on the RX 9060 XT.
- AOA works on the target phone.
- Pass: each item is answered yes or no with evidence. Any "no" triggers a design change before M1.

**M1: Virtual output**
- EDID generator (timing, physical size, name, serial, later chromaticity) with unit tests checked against `edid-decode`.
- Daemon creates, connects and disconnects a vkms connector through configfs.
- Pass: `niri msg outputs` shows the output with the right mode and physical size. Disconnect and reconnect keeps the same name.

**M2: Capture**
- Grab the scanout buffer, import as dmabuf, detect changed frames.
- Pass: a test pattern on the virtual output round-trips to a PNG that matches. Capture time per frame is logged.

**M3: Encode**
- VAAPI HEVC low-latency, output to a file.
- Pass: the file plays in `mpv` and `ffprobe` reports the expected stream. Encode time per frame is logged.

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
- NixOS flake and module: vkms, udev rules, systemd user service, kernel module options. Install docs.
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

- Daemon language. The leaning is C: DRM, VAAPI and libusb are C APIs, and it matches the author's systems background. Rust and Zig are possible. Decide at M0 once the spike code shows how much glue is needed.
- Share the protocol library between daemon and app as C (via JNI) or write it twice with a spec and tests. Decide at M4.
- Project name.
- Whether the daemon runs as a system service with `CAP_SYS_ADMIN` or as a user service with a small privileged helper. Decide at M2 when capture requirements are known.

## Risks

- vkms may lack per-connector EDID or hotplug control on the available kernel (unverified).
- Niri may not handle a secondary DRM device or vkms output well (unverified).
- Scanout capture gives no dirty rects, so change detection must be done by other means.
- The CPU copy into vkms buffers may limit resolution or refresh rate.
- AOA behavior varies by phone vendor firmware.
- Touch-to-output binding may fail on compositors without a binding mechanism.
- Compositors may not restore windows on output reconnect.
