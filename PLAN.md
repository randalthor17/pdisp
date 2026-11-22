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
3. **Encode**: VAAPI, HEVC 8-bit 4:2:0. Low-latency settings, no B-frames, CBR with a small VBV buffer to keep keyframes near P-frame size. No periodic IDR: send one on connect and on request, SPS/PPS with each. The application must write VPS/SPS/PPS and a slice header itself: Mesa parses them and rewrites its own (see `docs/encoder-notes.md`). Intra refresh is advertised by the driver but untested. A high-quality refresh frame is sent when the screen goes static. 10-bit, wide-gamut and AV1 come later. Colour range and matrix tags must be set deliberately.
4. **Transport**: Wi-Fi is the main path, and it must survive loss and congestion without stalling or corrupting the picture. Control (handshake, configuration, reconfiguration, clock sync) runs over TCP. Video and input run over UDP with application-level loss handling (see the protocol item). Android Open Accessory (AOA) over USB is the zero-setup wired path (libusb switches the phone to accessory mode, the app launches on plug-in, one permission tap) and carries the same packets, length-prefixed, over reliable bulk endpoints, where the loss handling stays idle. The transport interface reports queued bytes so the sender can skip frames before encoding.
5. **Protocol**: versioned handshake (min and max version, capability flags, extensible fields), then video, input, cursor, feedback and heartbeat messages. The phone reports resolution, density (for physical size in the EDID), orientation, safe-area insets, supported refresh rates, supported codecs, color capabilities (wide gamut, HDR types), and touch and stylus capability. The PC replies with the chosen mode and the parameter sets. Loss handling, all verified or designed in M0 and `docs/spikes/m0-loss-resilience.md`:
   - Video is split into packets of at most about 1200 bytes, with per-frame Reed-Solomon FEC (adaptive, roughly 10 to 20%), NACK retransmission for what FEC cannot repair, and paced sending.
   - Every video packet carries the frame id, the id of the frame it predicts from (or IDR), and the capture timestamp. The receiver feeds the decoder only frames whose reference it holds, and shows the last good picture otherwise (a short freeze, never garbage).
   - The receiver acknowledges frames. On loss the encoder predicts from the newest frame the receiver has (reference invalidation, implemented in `daemon/encoder.c`: bit-exact recovery without an IDR for up to 5 lost frames, an IDR beyond that, with the keyframe cost bounded by the small VBV buffer).
   - When the screen goes static the sender still sends a periodic keepalive carrying the latest frame id, so a lost final frame is noticed and repaired.
   - The receiver reports loss and delay trend about every 100 ms; the sender lowers the encoder bitrate and skips frames before encoding when its queue backs up. Latency stays bounded.
   - Input travels on its own UDP flow as pointer state (not deltas), repeated until acknowledged.
   - Packets are authenticated and encrypted with a session key from the handshake (M6), with the packet sequence number as the nonce.
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

**M3: Encode** (done 2026-11-21; direct libva, ~5 ms/frame at 2304x1440, see `docs/encoder-notes.md` and `tests/m3_encode_check.sh`)
- VAAPI HEVC low-latency from captured frames, output to a file. Prefer direct libva in the daemon; ffmpeg is acceptable for the first version.
- Pass: the file plays in `mpv` and `ffprobe` reports the expected stream. Encode time per frame is logged. The keyframe size stays near the largest P-frame size under the chosen CBR settings.

**M4: Protocol, Wi-Fi transport, desktop test client, network emulation, latency harness** (done 2026-11-21: the loss design passes its acceptance on the real pipeline under netem, `docs/stream-simulation.md`; RECONFIGURE, PING/PONG, clock sync, the live viewer window and the pixel-stamped glass-to-glass baseline are in, `docs/latency.md`)
- The protocol spec (`docs/protocol.md`, draft 0 awaiting review) with test vectors, and the shared C library: framing, handshake, packetisation, FEC, NACK, pacing, congestion feedback, reassembly. Fuzz and round-trip tests.
- TCP control plus UDP video and input. The encoder side uses `encoder_invalidate_from`, frame skipping before encode and bitrate adaptation.
- A desktop test client that decodes, shows the stream and enforces the "do not decode frames with a missing reference" rule.
- A network emulation harness: `netem` inside an unprivileged user and network namespace (verified in M0) for random and bursty loss, jitter, bandwidth caps and bufferbloat.
- A latency harness that stamps frames and measures glass-to-glass on the desktop.
- Pass: live view on the desktop. Under 2% bursty loss, 15 ms jitter and a 40 Mbit/s cap there is no persistent corruption, the picture recovers within a bounded time, and latency does not grow. Latency numbers recorded for the baseline.

**M5: Android app skeleton** (done 2026-11-21: the Tab S9 FE shows the monitor at 2304x1440, 60 fps over Wi-Fi; see `docs/spikes/m5-android.md`)
- Kotlin app: TCP connect, handshake with real resolution, density and refresh rates, `MediaCodec` surface decode.
- Pass: the phone shows the virtual monitor at its native resolution.

**M6: Pairing and security** (done 2026-11-21: Noise XX handshake with static keys, `authorized_keys` store with `pdisp-keys`, pairing prompt with a comparison code, encrypted control channel and datagrams; tests cover rejection and passwordless reconnect; verified on the Pixel 8a over Wi-Fi and USB; design in `docs/security.md`)
- Keypair, authorized-keys store, approval prompt, encrypted transport.
- Pass: an unauthorized key is rejected. An authorized key reconnects without a prompt. Tests cover both.

**M7: AOA transport** (done 2026-11-21 on the Tab S9 FE: plug in and the stream runs over the cable; see `docs/spikes/m7-usb.md`; done before M6 at the user's request, as it is the more reliable transport)
- libusb switch to accessory mode, bulk endpoints, udev rule, app launches on plug-in.
- Pass: plug in, tap permission once, and the monitor appears with no adb or developer mode.

**M8: Touch** (done 2026-11-21: finger touches become a uinput multitouch screen, the stylus a pen tablet, with palm rejection; verified with a Wayland client, a real GTK4 app (pinch-zoom and pan work) and a Pixel 8a; the niri binding is one config line each for touch and pen, see `docs/touch.md`. Not yet tried with a real pen or on the tablet)
- `uinput` multitouch device created from handshake capabilities. Touch messages over the protocol. Niri binding adapter.
- Pass: touches land on the phone's output, and compositor gestures (scroll, pinch) work.

**M9: Session lifecycle** (done 2026-11-22: grace period, removal on goodbye or lock, reuse on a quick return, app reconnect, USB pull and replug, windows on niri; `docs/lifecycle.md`, `docs/usb.md`)
- Grace period, hot-unplug, sleep and resume, lock.
- Pass: a scenario matrix on niri: unplug with windows open, resume with app open, resume with app closed, lock, daemon restart. Windows end up where the design says.

**M10: Quality and rates** (in progress 2026-11-22: text-quality measurement and static refinement done on loopback, see `docs/quality.md`; tablet decoder rate ladder done, `docs/latency.md`; decode-time feedback and Wi-Fi checks on the tablet and Pixel done; refresh policy and the real-text check open; 10-bit and wide gamut skipped because evdi is 8-bit only, see the end of `docs/quality.md`)
- Refresh rate mode selection, adaptive bitrate, static refinement, optional 10-bit and wide-gamut with matching EDID and stream color tags.
- Pass: visual check on text-heavy content. Latency under load stays within the recorded baseline plus a margin chosen at M4.

**M11: Packaging and 0.1 release** (in progress 2026-11-22: package, NixOS module, home-manager niri mapping, user service, pairing notification, `docs/install.md` and a NixOS VM check are done; a rebuild of the real system and the 0.1 tag are open)
- NixOS flake and module: evdi (`boot.extraModulePackages`, module options, device creation), udev rules, systemd user service. Install docs.
- The niri side is part of the install, not something to edit by hand: the touchscreen and tablet bindings (`input { touch { map-to-output "PNP(PDI) pdisp PDISP0001" } tablet { ... } }`, see docs/touch.md) go in through the home-manager module (the user's niri config is a read-only home-manager file with `dms/*.kdl` includes), and the install docs describe them for non-Nix setups. Touch must never reach other displays.
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
- Protocol library on Android: the FEC, reassembly, pacing and encryption are real logic, so the leaning is one C implementation used from Kotlin through JNI, rather than two implementations. Confirm at M5.
- Project name.
- Privilege model. Capture no longer needs `CAP_SYS_ADMIN`. The remaining privileged steps are loading the evdi module and adding a device (`/sys/devices/evdi/add`), so a user service plus a udev rule or a one-shot privileged setup unit (module options, device count) is the likely answer. Decide at M1.

## Risks

- evdi is out-of-tree. It must be rebuilt for each kernel (nixpkgs does this), and DisplayLink only claims verification up to 6.15. It worked on 6.18.63; newer kernels are unverified. If it breaks, the fallbacks are vkms plus hotplug on 6.19+ without EDID identity, or forcing a spare physical connector with `drm.edid_firmware`.
- evdi dirty rects are one bounding box per update, and full-screen when the whole output animates. They are a hint, not a precise region list.
- Compositor-to-evdi copy cost was not isolated. It kept up at 2304x1440@90 in M0, but heavier scenes and 4K are unmeasured.
- The tablet is on USB 2.0 high speed. That is enough for the measured HEVC stream (about 20 to 40 Mbit/s), but limits headroom for high bitrates, static refinement and 10-bit.
- The Mesa encoder only works with application-written packed headers that must agree with the VA parameters; a Mesa change in what it parses could break the stream. The integration test (`ffprobe`/decode) catches it. Sizes must be multiples of 8 until a conformance window is supported.
- Wi-Fi loss and congestion are the main product risk. netem is not real Wi-Fi (bursty loss, power save, aggregation), so real validation needs the tablet.
- Decoder behaviour on missing references differs by vendor. ffmpeg rejects every frame after one lost reference; Android MediaCodec is unknown until M5. The receiver rule above and reference invalidation avoid depending on concealment.
- Intra refresh is accepted by the driver and costs about 4 times the bits on moving content, but whether a decoder heals from it could not be checked with ffmpeg. It is a fallback, not the plan. Temporal layers are disabled by the driver when intra refresh is on, and are not used.
- The reference buffer holds 7 pictures, so loss windows beyond 5 frames (about 83 ms at 60 fps) need an IDR.
- GPU contention: hardware video decode running on the same GPU stalls hardware encode (it is time-sliced on the video engine): up to about 60 ms with an extreme 150 Mbit/s clip, negligible with a realistic 4K60 20 Mbit/s one (see `docs/spikes/m0-loss-resilience.md`). Compositing, 3D load and evdi are not the cause. The encoder must tolerate stalls (pipelining and frame skipping, M10). Using the iGPU encoder is possible but its conversion is slow.
- Keyframes on desktop content are about 7 times a P-frame unless rate control is constrained. Intra refresh is advertised but untested.
- AOA behavior varies by phone vendor firmware. The permission prompt and auto-launch on Android 16 are untested until the app exists.
- Touch-to-output binding may fail on compositors without a binding mechanism.
- Compositors may not restore windows on output reconnect.
