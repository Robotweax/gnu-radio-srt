# After 0.1.0-rc1: the lateness bound under netem, and two fixes

## Read this first

- Same limits as before: Linux netem in network namespaces on one
  virtualised aarch64 machine, sender and receiver on one host, reference
  sender and receiver. No real network, no second host, no hardware.
- The criteria below were written into
  [benchmarking.md](benchmarking.md) and committed before the runs. One
  trial run of each cell preceded that commit, to see that the harness
  worked; its numbers were of the same size and are not part of the
  results.
- Three repetitions per cell. This shows that the mechanism works in this
  emulated case, nothing more.

## The lateness bound (`max_lateness_ms`)

Milestone M4 found that the module does not bound the age of what it
delivers when a network path queues instead of dropping (profile N4: for
10 s the bottleneck carries 60 % of the stream in front of a very deep
queue). The Source can now discard a record that SRT hands over later than
a set time after its scheduled delivery.

Plan `rc2`: SRT, cf32 at 100 kS/s, latency 250 ms, 120 s per run, three
runs per cell. Ages are those of delivered samples in the measurement
window.

| Cell | Age p50 | Age p99 | Oldest delivered | Discarded as late | Usable on time |
|---|---|---|---|---|---|
| N4, no bound | 281 to 283 ms | 5895 to 5902 ms | 6256 to 6264 ms | 0 | 83.5 to 83.6 % |
| N4, bound 150 ms | 274 to 276 ms | 299 to 305 ms | 439 to 442 ms | 1.39 to 1.80 million samples | 83.5 to 84.8 % |
| N2, bound 150 ms | 296 to 302 ms | 297 to 303 ms | 304 to 308 ms | 0 | 99.92 to 99.98 % |
| N0, bound 150 ms | 253 ms | 254 ms | 256 to 283 ms | 0 | 100 % |

Against the criteria declared in advance:

| # | Criterion | Result |
|---|---|---|
| 1 | N4 with the bound: oldest delivered sample at most 525 ms old | **Met** in 3 of 3: 439 to 442 ms |
| 2 | N4 with the bound: every sample delivered or reported, no break of unknown size | **Met** in 3 of 3. In two runs every missing sample was one discarded as late. In one run 272 136 further samples were not delivered by SRT itself; they were reported as gaps like the others |
| 3 | N0 with the bound: nothing discarded as late | **Met** in 3 of 3 |
| 4 | N2 with the bound: at most 0.01 % discarded as late | **Met** in 3 of 3: nothing discarded |
| 5 | N4 without the bound shows ages of seconds again | **Yes**: up to 6.26 s, as in M4 |

All 12 runs also pass the existing gates of the protocol.

What this means:

- With the bound, what the Source delivers on this path is at most about
  0.44 s old instead of 6.3 s. The 17 to 18 s of signal per run that
  arrived too late are missing and reported as such.
- The share of windows that are complete and on time is the same with and
  without the bound. The bound does not rescue anything; it keeps stale
  samples out of downstream processing and says how many there were.
- Where the path keeps up, the bound costs nothing: the lateness of an
  on-time record was 2 to 4 ms, far below the bound.
- `max_lateness_seen_ms` showed about 5.98 s in all six N4 runs, with or
  without the bound. It is a direct sign that a path is queueing.

The bound is off by default. Turning it on changes what a receiver
delivers, and one emulated case is not enough to choose a default for
everyone.

## END sent three times

In [results-m5.md](results-m5.md), 1 of 6 runs of the blocks at 1 % loss
lost its END record, and the Source then waited out its 60 s reconnect
budget. SRT in live mode cannot recover the last packet of a connection.
The Sink now sends the END message three times, 10 ms apart.

- Automated test with the fault-injecting relay: with the first one or the
  first two END packets dropped, the run completes normally. With the
  repeats disabled the same test fails ("END not observed").
- With all END packets dropped the behaviour is as before:
  `termination_unknown`, never completion.
- A receiver stops reading at the first END, so it never sees a repeat.
  0.1.0-rc1 receivers are unaffected.
- The block run across N0 to N2 of M5 was repeated with this build, three
  runs of 120 s per path, encrypted: 9 of 9 pass, no END lost, every sample
  delivered or reported. Three runs at 1 % loss do not show that the
  repeats help; the relay test does that.

Summaries of all runs: [evidence/rc2/](evidence/rc2/). The generated
`netem/summary.md` puts the N4 runs with and without the bound into one
row; the table above and the per-run entries of `summary.json` keep them
apart.

The same build passes all four CI jobs on the hosted x86-64 runner (run
43401399656 on commit `701fcb1`): gate 11 of 11, netem harness check,
source package with the install guide in a fresh container, and the blocks
across N0 to N2.

## The receiver tells the sender why it ended a run

Found in the newcomer trial of M5: after a rate or format mismatch the
Source failed with a clear reason, and the Sink saw only `connection_lost`
(strict) or retried until its budget ended (segmented). The Source now
sends one small message back before closing.

- Automated tests, reference programs and blocks: for a rate mismatch and
  a format mismatch, with a strict and with a reconnecting Sink, the Sink
  ends with `configuration` and `reported by the peer: sample rate differs
  from configuration` (or format) within 5 s. A strict Source that sees a
  loss makes the Sink fail with `continuity` in the same way.
- Limits: the message can be lost; it reaches the Sink about two latencies
  after the first record left, so a shorter run completes before it
  arrives; 0.1.0-rc1 peers neither send nor read it.
- A found-and-fixed defect on the way: SRT refuses to hand over even a
  small message into a buffer smaller than the connection's payload size.

Not fixed: a Listener still shows no sign of a Caller with a wrong
passphrase. SRT rejects that Caller before the module is asked.
