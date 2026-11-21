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
