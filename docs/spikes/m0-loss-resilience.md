# M0 spike: loss resilience on Wi-Fi

Date: 2026-11-21. Host: NixOS 26.11, kernel 6.18.63, Mesa radeonsi 26.3.0 on the RX 9060 XT, ffmpeg 9.0.2 as the
decoder under test. Code: `daemon/encoder.c`, `tests/enc_synth.c`, `tests/loss_recovery_check.sh`.

Requirement: the tablet is mostly on Wi-Fi, and congestion or lost frames must not ruin the session. TCP and
"drop a frame, recover with an IDR" were ruled out (head-of-line blocking, latency growth, big keyframe bursts).
This spike tested what the design needs from the network emulation and the encoder.

## Network emulation without root: works

    unshare -Urn bash -c '
      ip link set lo up
      tc qdisc add dev lo root netem delay 10ms 3ms loss gemodel 1% 25% 80% 0.1% rate 40mbit
      iperf3 -s -1 -D; iperf3 -c 127.0.0.1 -u -b 20M -t 4 -l 1200'

- `netem` and `veth` work inside an unprivileged user and network namespace (the `sch_netem` module was loaded).
- A 2% uniform loss setting measured 2.1% (172 of 8335 packets). The Gilbert-Elliott model measured 3.1% with
  correlated drops, closer to Wi-Fi. Delay, jitter and a rate cap are supported.
- Real Wi-Fi (power save, aggregation, roaming scans) is not reproduced; real validation needs the tablet at M5.

## Encoder features on this driver

| Feature | Result |
|---|---|
| Multiple slices | Works. 8 slices per picture, every frame, decodes with no errors. `slices` in the encoder config. |
| Intra refresh (rolling column, `VAEncMiscParameterRIR`) | Accepted. Costs about 4x the bits on moving content (3.2 MB vs 0.78 MB for 300 frames of scrolling pattern). **Healing could not be verified**: ffmpeg does not conceal (see below). |
| Temporal layers | Not used. Mesa disables intra refresh whenever temporal layers or B-frames are on, and intra refresh is the fallback we want. |
| Reference invalidation (choose an older reference) | **Works, bit-exact.** See below. |

Mesa facts that shaped this (from `radeon_vcn_enc.c`): intra refresh is specified in CTU columns or rows with the
application advancing the offset each frame; the region is widened by one unit for loop-filter overlap.

## Why the old bitstream could never recover

Every P-frame's slice header named only `POC-1` in its reference picture set. In HEVC, pictures not in the current set
are dropped from the decoder's buffer, so after one lost frame there is nothing left to predict from and a conformant
decoder cannot recover without an IDR. ffmpeg shows it: after the loss it logs `Could not find ref with POC N` and
`Error constructing the frame RPS` for every following frame and outputs nothing new. This also means ffmpeg is not a
usable judge of concealment or intra refresh healing; only of streams whose references are intact.

Fix: the encoder keeps 7 reconstruction surfaces (the current picture and up to six retained references). Every slice
header lists all valid retained pictures in the reference picture set, exactly one marked used by the current picture.
`VPS/SPS max_dec_pic_buffering` is 7. The decoder keeps all of them.

## Reference invalidation

`encoder_invalidate_from(enc, first_lost)` marks frames from `first_lost` on as not held by the receiver. The next frame
predicts from the newest valid retained frame (an older one), and the reference set no longer lists the lost pictures.
If none is left the next frame is an IDR. Test: `enc_synth -m -X a:b` scrolls the pattern (every block changes), tells the
encoder frames a..b were lost, then the lost frames are cut out of the stream and the damaged stream is compared with
the clean one frame by frame (`framemd5`).

| Frames lost | Frame after the loss | Result |
|---|---|---|
| 2 (100-101) | predicts from 3 back | bit-identical, 0 decoder errors |
| 5 (100-104) | predicts from 6 back (the limit) | bit-identical, 0 decoder errors |
| 6 or more | IDR (no reference left) | bit-identical, 0 decoder errors |

The 6-frame case first failed: the current picture overwrote the slot of its only valid reference. The encoder now frees
the slot first and falls back to an IDR if no reference remains. `tests/loss_recovery_check.sh` runs three cases
(4, 5 and 11 lost frames) in the suite.

## Consequences for the protocol

- **The receiver must not feed the decoder a frame whose reference it does not hold.** ffmpeg shows what happens
  otherwise (permanent failure); other decoders conceal differently. Every video packet therefore carries its frame id
  and the id of the frame it predicts from; the receiver freezes on the last good picture until a frame with a held
  reference arrives. A short freeze instead of garbage, independent of decoder concealment.
- The receiver acknowledges frames; the sender calls `encoder_invalidate_from` on a reported loss. Frames encoded
  between the loss and the feedback arriving (about one round trip) are the ones the receiver discards.
- The recoverable window is 5 frames, about 83 ms at 60 fps. Longer outages cost an IDR, whose size is bounded by the
  small rate-control buffer. A deeper buffer (the level and the 15-entry VA list allow it) is untested.
- A static screen produces no frames, so loss would go unnoticed. The sender needs a periodic keepalive with the latest
  frame id, and a repair path for the final frame.

## Observed, not isolated

While mpv decoded and rendered fullscreen on the same GPU, encoder time rose from 2 to 4 ms to 7 to 8 ms per frame with
spikes above 100 ms (`encode_ms max=122`). Likely GPU contention with the compositor and video decode. This matters when
the main monitor is busy (a game). Needs its own measurement, including whether queue priority can be raised.
