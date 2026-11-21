# pdisp protocol, version 1 (draft 0, for review)

Status: draft for review, not implemented. Nothing here is final until the open questions at the end are settled.
The design follows from the M0 spikes (`docs/spikes/`): Wi-Fi is the main transport, so loss and congestion must not
stall or corrupt the picture, and the encoder can recover from loss without a keyframe
(`encoder_invalidate_from`, bit-exact for up to 5 lost frames).

All integers are little-endian. "PC" is the daemon, "phone" is the Android app (also a tablet).

## 1. Layers and channels

| Channel | Transport | Direction | Carries |
|---|---|---|---|
| Control | TCP, one connection | both | handshake, setup, reconfiguration, cursor shapes, liveness, close |
| Media and feedback | UDP, one socket pair | both | video, cursor position, keepalive, feedback, input, time sync |

- The phone opens both: it connects the TCP control channel, then sends the first UDP packet. The PC learns the phone's
  UDP address from that packet (firewall and NAT friendly) and answers from the same socket.
- Control is low volume and reliable, so TCP head-of-line blocking does not matter. Video, input and feedback are
  loss-tolerant and latency-critical, so they never share a TCP stream.
- Over AOA (USB), both channels are carried on the reliable bulk endpoints: control messages and UDP packets are each
  prefixed with a `u16` length. FEC, NACK and retransmission stay idle there (the receiver still acknowledges frames).
- Security (milestone M6) wraps both channels: a handshake authenticated by the phone's key, then per-packet
  authenticated encryption on UDP (section 3.4). Milestone M4 uses a `security = none` mode that exists for development only.

## 2. Control channel (TCP)

### 2.1 Framing

    u32 length     bytes after this field (type + flags + payload), at most 1 MiB
    u16 type
    u16 flags      reserved, 0
    ... payload

Unknown message types are skipped by length. Receivers must never read past `length`.

### 2.2 Messages

| Type | Name | Direction |
|---|---|---|
| 0x0001 | HELLO | phone to PC |
| 0x0002 | SETUP | PC to phone |
| 0x0003 | REJECT | PC to phone |
| 0x0004 | READY | phone to PC |
| 0x0010 | RECONFIGURE | PC to phone |
| 0x0011 | RECONFIGURE_ACK | phone to PC |
| 0x0012 | DISPLAY_CHANGED | phone to PC |
| 0x0020 | CURSOR_SHAPE | PC to phone |
| 0x0030 | PING | both |
| 0x0031 | PONG | both |
| 0x00ff | CLOSE | both |

Strings are `u16 length` + UTF-8. Lists are `u16 count` + elements. Every HELLO, SETUP and RECONFIGURE ends with
extension TLVs (`u16 type, u16 length, value`); unknown TLVs are skipped, which is how fields are added without a version bump.

**HELLO** (phone to PC, first message after the (M6) secure handshake):

    u16 proto_min, u16 proto_max         supported protocol versions, this document is version 1
    string device_name
    u32 device_caps                      bit0 touch, bit1 stylus, bit2 hdr10, bit3 wide gamut, bit4 hlg
    u16 native_width, native_height      pixels, in the orientation the app will display
    u16 density_dpi                      used for the EDID physical size
    u8  orientation                      0 landscape, 1 portrait, 2 reverse landscape, 3 reverse portrait
    u16 inset_left, inset_top, inset_right, inset_bottom   safe-area insets in pixels
    list<u32> refresh_mhz                supported refresh rates in millihertz, e.g. 60000, 90000, 120000
    list<u8>  codecs                     1 = HEVC Main 8-bit (required in v1); 2 = AV1, 3 = HEVC Main10 are reserved
    u8  max_touch_points
    u16 max_udp_payload                  largest UDP payload the phone will accept, at least 1200
    string app_version

**SETUP** (PC to phone, answer to HELLO):

    u16 proto_version                    chosen, within the phone's range
    u32 session_id                       random, identifies this session on UDP
    u16 udp_port
    u8  epoch                            starts at 0
    u8  security                         0 none (M4 only), 1 chacha20-poly1305 (M6)
    u16 width, height                    the mode of the virtual output (pixels), may differ from native
    u32 refresh_mhz
    u8  codec                            1 = HEVC Main 8-bit
    u8  color_range, color_primaries, color_transfer, color_matrix    H.273 codes; v1: 0 (limited), 1, 13, 1
    u16 shard_size                       bytes of video payload per UDP packet, at most max_udp_payload minus 60
    u8  fec_percent                      initial parity overhead
    u32 bitrate_kbps, min_bitrate_kbps, max_bitrate_kbps
    u8  dpb_frames                       references the encoder retains (7 in v1, so up to 5 lost frames recover without an IDR)
    bytes parameter_sets                 VPS, SPS and PPS, Annex B (codec-specific data for MediaCodec)
    ... TLVs

**REJECT**: `u16 reason` (1 no common version, 2 not authorised, 3 busy, 4 unsupported display), `string text`.
**READY**: empty. The phone has opened its UDP socket and configured its decoder; the PC then starts video with an IDR.

**RECONFIGURE** (PC to phone): the fields of SETUP from `epoch` on, with the `epoch` incremented, new parameter sets and
mode. The PC sends no video of the new epoch until it receives RECONFIGURE_ACK. The first video frame of an epoch is an IDR
and carries the `new_epoch` flag. Packets of an older epoch are discarded.
**DISPLAY_CHANGED** (phone to PC): the HELLO display fields again (rotation, new insets, window size). The PC decides whether
to change the virtual output (EDID reconnect or mode change) and replies with RECONFIGURE.

**CURSOR_SHAPE**: `u16 shape_id, i16 hot_x, i16 hot_y, u16 width, u16 height, bytes argb` (at most 128 x 128 x 4 bytes).
Cursor positions use shape ids (section 3.7).

**PING/PONG**: `u64 token` echoed back. Either side may send them; if nothing is received on TCP for 5 s the channel is dead.
**CLOSE**: `u16 reason` (0 normal, 1 screen lock, 2 replaced by another phone, 3 error), `string text`.

### 2.3 Session establishment

    phone                                   PC
      TCP connect  ------------------------>
      (M6: secure handshake, device authorised)
      HELLO  ------------------------------>
                                            creates or reconnects the virtual output from the HELLO display fields
      <------------------------------ SETUP   (or REJECT)
      opens UDP, configures the decoder
      UDP: HELLO_UDP {session_id} ---------->
      READY (TCP)  ------------------------->
      <---------------------- UDP: IDR video, KEEPALIVE
      <--------------------------> steady state

The virtual output (and its EDID, from the HELLO display fields and the paired device key) exists before SETUP is sent,
because SETUP carries the real mode; the compositor modeset takes about a second, so the phone shows "connecting".

## 3. Media channel (UDP)

### 3.1 Common packet header (16 bytes, then the payload, then the 16-byte AEAD tag when `security = 1`)

    u32 session_id
    u8  type
    u8  flags         bit0 retransmission or repair packet, others reserved
    u16 reserved      0
    u64 seq           per sender and per session, increments by one for every packet sent, never reused; used as the AEAD nonce

Datagrams with a wrong `session_id`, an unknown `type`, or a `seq` already seen (replay window of 1024) are dropped.
Maximum datagram size is `shard_size + 60`.

Example, a VIDEO packet header with session 0x11223344 and seq 1:
`44 33 22 11 01 00 00 00 01 00 00 00 00 00 00 00`.

| Type | Name | Direction |
|---|---|---|
| 0x00 | HELLO_UDP | phone to PC |
| 0x01 | VIDEO | PC to phone |
| 0x02 | CURSOR_POS | PC to phone |
| 0x03 | KEEPALIVE | PC to phone |
| 0x10 | FEEDBACK | phone to PC |
| 0x11 | INPUT | phone to PC |
| 0x12 | INPUT_ACK | PC to phone |
| 0x14 | TIME_REQ | phone to PC |
| 0x15 | TIME_RESP | PC to phone |

### 3.2 VIDEO packets

One encoded frame (one HEVC access unit, Annex B) is split into data shards of `shard_size` bytes (the last one padded with
zeros) and protected with Reed-Solomon parity shards over GF(2^8) (systematic, Cauchy matrix). A frame of at most 255
shards is one block; larger frames are split into blocks of at most 255 shards each (data plus parity). Any `data_shards`
of the `data_shards + parity_shards` shards of a block reconstruct it.

    u32 frame_id        0, 1, 2, ... within the session; every encoded frame consumes one id (skipped captures do not)
    u32 ref_frame_id    the frame this one predicts from, or 0xFFFFFFFF for an IDR
    u64 capture_us      sender clock, microseconds, when the frame was captured
    u32 frame_bytes     length of the access unit without padding
    u8  epoch
    u8  block_index
    u8  block_count
    u8  shard_index     0 .. data_shards-1 are data; data_shards .. 254 are parity
    u8  data_shards
    u8  parity_shards   shards sent initially; repairs may use indices beyond this up to 254
    u8  vflags          bit0 IDR, bit1 new_epoch, bit2 refresh (a re-encode of an unchanged screen after a loss)
    u8  reserved
    ... shard bytes (shard_size)

Every packet carries all fields, so any single packet identifies its frame, block and the geometry needed to decode it.

Sender rules:

- Packets are paced across at most one frame interval (never sent as one burst) and parity follows the data immediately.
- `parity_shards = max(1, ceil(data_shards * fec_percent / 100))` when FEC is on. `fec_percent` adapts to the reported loss.
- The sender keeps the shards of the last 250 ms of frames to answer NACKs.
- Before encoding, if the previous frame's packets are still queued, the sender skips the capture and encodes the newest one
  later (section 5).

### 3.3 Receiver rules (frame gating)

The receiver reassembles blocks with the FEC decoder, then decides per completed frame:

1. If the frame is an IDR, or its `ref_frame_id` is in the set `held` of frames the decoder already accepted, submit it to the
   decoder, add `frame_id` to `held`, and show the result.
2. Otherwise do not decode it. The last good picture stays on screen (a short freeze, never garbage) and the frame is
   reported as not held.

`held` covers the last 64 frames. Frames older than the newest IDR minus one epoch are forgotten. A decoder error clears
`held` and sets the `need_idr` feedback flag.

This is what makes the design independent of how a given Android decoder conceals missing references; ffmpeg, for example,
rejects every frame after one lost reference (`docs/spikes/m0-loss-resilience.md`).

### 3.4 Authenticated encryption (M6, informative)

Each direction has its own key from the handshake. The AEAD is ChaCha20-Poly1305 with the 96-bit nonce formed from a
fixed direction byte and the 64-bit `seq`; the common header is the associated data. `seq` therefore never repeats for a
key. The control channel is protected by the same handshake. Details are fixed in M6.

### 3.5 FEEDBACK (phone to PC)

Sent whenever a frame completes or is lost, and at least every 100 ms. At most 16 NACK entries; if there are more, the
oldest frames are given up.

    u8  flags           bit0 need_idr
    u32 newest_frame_id        highest frame id the phone has seen any packet of
    u32 held_base              every frame id up to and including this one that exists is held or given up
    u64 held_bitmap            bit i set: frame held_base + 1 + i is held
    u8  nack_count
    ... per NACK: u32 frame_id, u8 block_index, u8 need     shards still missing to reconstruct that block
    u16 loss_permille          packets lost since the last report, after FEC
    i32 delay_trend_us_per_s   estimated queueing delay slope (positive means the queue is growing)
    u32 recv_kbps              received rate over the report interval
    u32 jitter_us
    u16 decode_ms_x10          recent average decode time

The PC answers a NACK by sending `need` new parity shards (indices beyond the initial parity) with the retransmission flag set.
A frame the phone has given up on appears in `held_bitmap` as not held once `held_base` has passed it.

### 3.6 KEEPALIVE (PC to phone)

Sent every 100 ms in which no VIDEO packet was sent:

    u32 newest_frame_id
    u64 now_us          sender clock
    u32 input_acked     highest input event seq applied (see 3.8)

A phone that sees `newest_frame_id` above any frame it has data for knows a trailing frame was lost on a static screen and
reports it as a NACK. Missing KEEPALIVEs for 2 s means the link is lost.

### 3.7 CURSOR_POS (PC to phone)

    u16 shape_id        0 = hidden; otherwise a shape delivered by CURSOR_SHAPE
    u16 x, y            normalised over the virtual output, 0..65535
    u32 move_seq

Latest wins; loss is harmless. Positions are sent at the cursor event rate, at most one per millisecond.

### 3.8 INPUT and INPUT_ACK (phone to PC, and back)

Touch and stylus events are an ordered, reliable stream carried by redundancy, never by retransmission delay:

    u8  count
    ... per event, oldest first:
        u32 event_seq       1, 2, 3, ... per session
        u8  kind            0 down, 1 move, 2 up, 3 cancel, 4 hover (stylus), 5 button (stylus)
        u8  pointer_id
        u8  tool            0 finger, 1 stylus, 2 eraser
        u8  buttons
        u16 x, y            normalised over the phone's display, 0..65535
        u16 pressure        0..65535
        i8  tilt_x, tilt_y  degrees
        u32 time_us         phone clock

Each INPUT packet repeats every event not yet acknowledged (up to 32, oldest dropped as a last resort). The PC applies events in
`event_seq` order, ignores ones it has seen, and acknowledges with INPUT_ACK `{u32 input_acked}` (also carried in
KEEPALIVE). The phone stops repeating acknowledged events. A lost packet therefore never leaves a finger stuck down, and input
never queues behind video.

### 3.9 Time sync

TIME_REQ `{u64 t1}` (phone clock). TIME_RESP `{u64 t1, u64 t2, u64 t3}` where t2 is the PC receive time and t3 the PC send time.
The phone computes the offset and round trip as in NTP and keeps the sample with the lowest round trip over the last 10
seconds. Sent every second. Used to turn `capture_us` into glass-to-glass latency and to schedule presentation.

## 4. Handshake versioning and compatibility

- The phone states a range `[proto_min, proto_max]`; the PC picks the highest version it supports in the range or REJECTs.
- Unknown message types, TLVs and UDP packet types are skipped. Reserved fields are sent as 0 and ignored.
- A change that breaks parsing of existing messages needs a new major version in the range; adding messages or TLVs does not.

## 5. Sender behaviour (informative)

- **Frame skipping**: the daemon asks the transport how many bytes are queued. If the previous frame is still being sent, it
  skips the new capture before encoding (an encoded frame cannot be dropped without breaking the reference chain).
- **Reference invalidation**: when a frame has not been acknowledged as held after about 1.5 round trips plus the repair
  attempts, the daemon calls `encoder_invalidate_from(frame_id)`; the next frame predicts from the newest frame the phone holds.
  If none is left (more than 5 frames lost) it is an IDR. On a static screen the daemon re-encodes the current screen as a
  `refresh` frame; it is tiny because nothing changed.
- **Rate control**: loss, delay trend and received rate drive the target bitrate (loss-based decrease, delay-based back-off,
  gentle increase), applied through the encoder rate control, within `[min_bitrate, max_bitrate]`. Latency must not grow:
  when the queue is non-empty the bitrate is reduced, never buffered.
- **GPU stalls**: encode can stall for tens of milliseconds while hardware video decode runs on the same GPU; frame skipping
  and the rate controller absorb it.

## 6. Not covered by this draft

- Device discovery (manual host and port in M4; mDNS later) and the AOA accessory strings.
- The secure handshake and key derivation (M6).
- The congestion control algorithm beyond the wire fields above.
- Audio, clipboard, multiple phones (not in v1).
- HDR and 10-bit signalling beyond the reserved codec and capability values.

## 7. Verification plan

- A C reference library in `proto/` implements encode and decode of every message, the packetiser and the FEC. Unit tests,
  round trips, and a libFuzzer or AFL target on the parsers (control and UDP) with the size limits above.
- Golden test vectors in `proto/vectors/` (bytes plus decoded fields), produced by the C library and consumed by the Android
  tests, so both sides stay in sync even if the app reimplements parts.
- A network emulation harness (`netem` in an unprivileged user and network namespace): random and bursty loss, jitter,
  bandwidth caps, bufferbloat. Acceptance: under 2% bursty loss, 15 ms jitter and a 40 Mbit/s cap there is no persistent corruption, the
  picture recovers within a bounded time, and latency does not grow.
- The desktop test client enforces the frame-gating rule and logs held, skipped and recovered frames.

## 8. Open questions

1. **Packet overhead.** Fixed overhead is 16 (header) + 28 (video header) + 16 (tag) = 60 bytes per 1200-byte shard, 5%.
   Acceptable, or compact the video header (for example, send `capture_us` and `frame_bytes` only in shard 0 of block 0)?
2. **FEC defaults.** Start at 10% parity, adaptive between 0 and 30% from `loss_permille`?
3. **Shard size.** 1200 bytes avoids fragmentation on Wi-Fi (MTU 1500) and tunnels. Larger shards (1400) cut overhead and
   packet rate but risk fragmentation.
4. **Input.** Redundant event stream (as specified) versus a state snapshot. Events keep taps and fast gestures exact; the
   cost is a slightly larger packet while events are unacknowledged.
5. **Cursor.** Drawn on the phone from CURSOR_POS and shapes (as specified), versus baked into the video. The cursor then
   has no encode latency, but cursor effects the compositor draws itself are not shown.
6. **Library sharing.** One C library used from Kotlin through JNI (leaning), or reimplemented with the vectors as the check.
7. **Codecs.** HEVC Main 8-bit only in v1, with AV1 and Main10 reserved in the codec list.
