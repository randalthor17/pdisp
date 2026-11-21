# Stream simulation (M4)

`tests/test_stream.c` runs the real sender (`proto/vsend.c`) and receiver (`proto/vrecv.c`) over a simulated link, entirely
in process: a simulated clock, a seeded generator, no sockets. It is the main regression test for the loss design in
`docs/protocol.md`.

## What is simulated

- **The link**, in both directions: fixed delay, jitter (which reorders), independent or Gilbert-Elliott bursty loss,
  blackout windows, and a bandwidth cap with a tail-drop queue.
- **An encoder model** with the same reference rules as `daemon/encoder.c`: predict from the newest valid frame at most 6
  back, an IDR when none is left, a per-frame `invalidate` hook. Frames are 60 per second, sized from the sender's target
  bitrate, with IDRs three times larger. A capture is skipped before encoding when the sender's queue is more than 20 ms deep.
- **A decoder model** that keeps the picture of every frame it decoded and checks each new one against ground truth. A frame
  decoded with a missing or wrong reference produces garbage and counts as `corrupt`, so "no corruption" is a result, not an
  assumption. It also checks the reassembled payload byte for byte.
- A real event loop: the sender is woken at each packet's due time, not once per simulation step.

## Results (8 s runs, 60 fps, 20 Mbit/s target, 5 ms each way unless stated)

| Scenario | Frames displayed | Longest freeze | Latency p50 / p99 | IDRs | Corrupt |
|---|---|---|---|---|---|
| Clean link | 100% | 21 ms | 16.5 / 19 ms | 1 | 0 |
| 2% loss, 8 ms +/- 4 ms jitter (5 seeds) | 100% | 22 to 23 ms | 24 / 28 ms | 1 | 0 |
| Bursty loss, ~3%, bursts of 5 (5 seeds) | 100% | 51 to 63 ms | 19 / 43 to 50 ms | 1 | 0 |
| 70 ms blackout | 99.3% | 101 ms | 16.5 / 69 ms | 1 | 0 |
| 400 ms blackout | 86.7% | 568 ms | 16.5 / 425 ms | 2 | 0 |
| 10 Mbit/s cap, 30 Mbit/s start | 100% | 24 ms | 26 / 110 ms | 2 | 0 |
| Last frame lost, static screen | every frame, including the last | n/a | n/a | n/a | 0 |
| 2% loss, 20% feedback loss | 100% | 41 ms | 17 / 19.5 ms | 1 | 0 |
| Decoder error at 4 s | 99.5% | 67 ms | 17 / 19 ms | 2 | 0 |

- After a 70 ms blackout the picture is back 32 ms after the link returns, without an IDR: the frames lost in the outage are
  resent whole, because they are small.
- After a 400 ms blackout it is back 42 ms after the link returns, with one extra IDR (the outage outlasts every retained
  reference). The 568 ms freeze is the outage plus 168 ms.
- Under the cap the controller converges from 30 Mbit/s to about 7 Mbit/s and latency stays bounded.
- The decoder error costs exactly one IDR.

## What it took to get here (bugs this harness found)

- Repairs were requested 3 ms after a frame was first seen, but pacing spreads a frame over about 11 ms: 3,492 false repair
  packets on a clean link. NACKs now fire when a frame has been **idle** for 4 ms.
- The congestion signal (delay trend) was dominated by frame-size variation and collapsed the bitrate on a clean link. It
  is now queueing delay: the median one-way delay of the **first packet** of recent frames minus the minimum over about 2 s,
  and the controller reacts only to persistent congestion.
- `invalidate_from(first_lost)` also dropped a freshly sent IDR, causing extra keyframes. The sender now invalidates each lost
  frame individually (`encoder_invalidate_frame`); the receiver already gives up on every dependent frame.
- `need_idr` was answered on every report, producing IDR storms. It is acted on once per 150 ms, and ignored when an IDR
  was just sent.
- The spec's `held_base` could not tell held frames from given-up ones; the meaning was fixed (see section 3.5 of the spec).

## Limits

- It is a model. Real Wi-Fi has bursty loss, power save and rate changes that this only approximates; the tablet at M5 is
  the real test. The loss parameters above are plausible, not measured.
- Under mild loss the controller sometimes settles below the maximum (10 to 20 Mbit/s at 2% loss with jitter) although FEC
  alone covers that loss. It is safe but not optimal; tune it against real measurements.
- The decoder is a model: it assumes a decoder that decodes exactly the frames it is given, with the references the sender
  signalled. Real MediaCodec behaviour is checked at M5.
- Congestion control is intentionally simple. A stronger algorithm can replace `adapt()` without changing the wire format.

## Found by the real loopback run (not by the simulation)

The first run of `pdispd` and `pdisp-view` through the real compositor, encoder and sockets (`tests/m4_loopback_check.sh`)
decoded 40 fps with 44 frames given up and a collapsed bitrate. Three causes, none of which the simulator showed:

- **The delay signal included the sender's own encode time.** The one-way delay was measured from the frame's capture time,
  so a 19 ms encode looked like network queueing and the controller cut the bitrate. Every video packet now carries `send_us`,
  the sender's clock when the packet left the pacer, and the receiver measures network delay from that (video header is
  32 bytes, overhead 64 bytes per 1200-byte shard).
- **A doomed reference chain.** When an IDR is damaged, the sender keeps encoding on its newest frame, which it still thinks
  is valid because the loss report has not arrived yet; the next report always lags the newest frame, so the chain never
  breaks and nothing is ever decoded again. Reproduced in the simulation (10 Mbit/s cap with a 187 KB first IDR, zero frames
  decoded in 13 s). `vsend_must_idr()` forces an IDR when too many frames are unresolved (more than 8, or about twice the
  round trip in frames), with a 150 ms cooldown, and not while the receiver's newest frame id has stopped advancing (a dead
  forward path, where a keyframe cannot help).
- **The pacer sent one packet per wake-up.** The next send time was `max(next_due, now)`, so waking late forfeited the
  accumulated credit; at millisecond timer granularity that caps the rate near 10 Mbit/s whatever the target. The pacer now
  allows catching up by 2 ms of credit. The simulation now services the sender once per 500 us step like a real loop, so
  this is tested.

After the fixes, on the real pipeline (2304x1440 at 60 Hz, loopback, animated pattern, 10 s): 580 frames decoded at 58 fps,
nothing given up, one IDR, 82 repair packets, capture-to-decode latency p50 20 ms, p99 31 ms (software decode averages
7.75 ms per frame), decoded picture matching the pattern.

## M4 acceptance on the real pipeline: netem (`tests/m4_netem_check.sh`)

The real daemon and viewer, through niri and evdi, inside an unprivileged user and network namespace with netem on the
loopback device, applied to UDP only (the control connection stays on a clean path). 10 s per scenario, animated pattern
at 2304x1440 and 60 Hz, ~600 frames. The viewer checks the decoded picture against the pattern every 20 frames, so
"no persistent corruption" is measured throughout the run, not only at the end.

| Scenario | Frames decoded | Given up | IDRs | Longest freeze | Latency p50 / p99 | Corrupt pictures |
|---|---|---|---|---|---|---|
| 8 ms delay each way | 572 to 579 | 0 | 1 | 39 to 47 ms | 16 to 26 / 38 to 57 ms | 0 |
| bursty ~2% loss, 15 ms jitter, 40 Mbit/s cap (the plan's acceptance case) | 563 to 578 | 0 (9 in one of six runs) | 1 (2) | 89 to 251 ms | 44 to 49 / 102 to 115 ms | 0 |
| bursty ~5% loss, 15 ms jitter | 563 to 576 | 0 | 1 | 86 to 108 ms | 33 to 35 / 88 to 94 ms | 0 |
| 15 Mbit/s cap, target starting at 20 | 563 to 579 | 0 | 1 | 51 to 61 ms | 33 to 40 / 90 to 105 ms | 0 |

All of it passes: no persistent corruption (every periodic check at 95.8% or better against a pattern whose animated bar covers
at most 3%), freezes bounded at about a quarter of a second at worst, and latency that does not grow. The rare 250 ms freeze is
the expected recovery cost of an unrepaired loss: 100 ms before the receiver gives up, a round trip, and an IDR.

### What the netem runs found

The first runs failed badly (138 frames decoded under a 15 Mbit/s cap; a 6 s freeze under 5% loss), which led to three
changes in the sender. The simulation had not shown any of them because its link was gentler and its loss less bursty.

- **Controller oscillation.** The target went 7.9, 18, 24, 19 Mbit/s on a 15 Mbit/s link: it increased about 40% a second but
  took five reports to notice a queue and cut 7%. Each overshoot filled the 100 ms buffer, the round trip rose to 90 ms, and
  every loss event then took about six frames with it. The controller now increases about 2% per 100 ms and only after a quiet
  second, cuts within three reports in proportion to the queue depth, and starts at 12 Mbit/s.
- **Loss is not congestion.** The per-interval loss figure covers about 14 packets, so one lost packet reads as 71 permille.
  Using it for decisions made the bitrate drift to the floor under sustained random loss. A smoothed figure (about 200 ms)
  now gates increases above 10% loss and triggers cuts only above 20%; delay is the congestion signal, and the FEC overhead
  (up to 30%) handles random loss.
- **The doomed chain, again.** From 2.5 s on every frame was lost, although the sender saw only 3 or 4 frames unresolved:
  lost frames count as resolved, and each report invalidated one frame while the encoder kept predicting from its newest valid
  frame, a not-yet-reported descendant of the lost one. The sender now also invalidates all descendants of a lost frame at
  once (it knows every frame's reference, and the receiver gives up on descendants anyway), which breaks the chain at the source.

### Limits

- netem is not Wi-Fi: loss is bursty here, but there is no power save, no rate adaptation, no aggregation, and the paths are
  symmetric. The tablet decides.
- The viewer decodes in software. A phone uses MediaCodec; decode latency differs.
- Run-to-run variation is real (the freeze in the acceptance case ranged 89 to 251 ms), so the thresholds are bounds, not
  targets: 400 ms for freezes in the loss scenarios.

## The "corrupt picture" failures were the desktop, not the codec (2026-11-21)
The netem acceptance test failed about one run in four with "N periodic pattern checks found a corrupt picture (lowest match 0.0%)",
in whichever scenario was running, including the one with no loss at all, while the viewer reported 0 decoder errors, 0 given-up frames
and one IDR. `pdisp-view -o snap.png` now saves the first three mismatching pictures next to the snapshot. They showed niri's overview
(the pattern window zoomed out among the wallpaper): a stray touch of a hot corner or a key on the machine running the tests. The
decoder reproduced it faithfully. Pictures that match almost nowhere (under 5%) are now reported as `check_other` (and as a NOTE),
not counted as corruption, and a run with both such pictures and partial mismatches (the overview's open and close animations) is
skipped as inconclusive. Partial mismatches alone, which is what a decode error looks like, still fail the test. Previously widened
timing limits remain; run-to-run variance there is real.
