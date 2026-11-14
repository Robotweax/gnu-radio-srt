# First run between two machines: plan `two-host-1`

## Read this first

- One run, on 14 November 2026: sender on a cloud virtual machine in
  Frankfurt, receiver on one in Berlin, over the provider's path between the
  two regions. Two kernels, two clocks. Everything before this ran on one
  machine with one clock.
- The plan and its criteria were written into
  [benchmarking.md](benchmarking.md) and committed (`96b7cc4`) before the run
  was started. The run was started once and is reported as it came out.
- The machines were created, driven and removed by a private test harness of
  the maintainer. It is not part of this repository. This page and
  [evidence/two-host-1/summary.json](evidence/two-host-1/summary.json) hold
  figures derived by that harness's validator; the raw records, addresses and
  run identifiers stay with the maintainer.
- The reference sender and receiver, not the GNU Radio blocks. Plaintext.
  One direction, one rate, one day, shared virtual machines.

## Setup

Two machines with 2 vCPU and 4 GiB, Ubuntu 24.04 x86-64 (g++ 13.3.0). Both
built Robotweax SRT 0.3.2 (`e45d0edd`) and the reference peers from the
published 0.1.0-rc2 source (`c25c678`, content hash `bbbe05e1…4210`). The two
builds were byte-identical. cf32 at 100 kS/s, latency 250 ms, 120 s per case,
segmented continuity, `max_lateness_ms` off. Round trip between the machines:
9.6 to 10.2 ms.

## Results

Ages are in milliseconds and taken at the clock bound: every sample is counted
as old as the clock error allows. "Usable" is the share of 1024-sample windows
that were complete and on time (deadline 400 ms).

| Case | Path | Delivered | Usable | Age p50 | Age p99 | Oldest | Clock bound | Packets lost / resent |
|---|---|---|---|---|---|---|---|---|
| `wan-1` | as it is | 100 % | 100 % | 263.1 | 264.1 | 281.5 | 5.1 | 0 / 0 |
| `loss-1` | + 20 ms, 1 % loss | 100 % | 100 % | 283.1 | 284.2 | 296.8 | 5.4 | 908 / 913 |
| `wan-2` | as it is | 100 % | 100 % | 262.6 | 263.3 | 272.3 | 5.3 | 0 / 0 |
| `loss-2` | + 20 ms, 1 % loss | 100 % | 100 % | 283.2 | 284.1 | 292.6 | 5.7 | 879 / 887 |
| `wan-3` | as it is | 100 % | 100 % | 263.4 | 264.2 | 275.4 | 5.2 | 0 / 0 |

Without the clock bound the ages are 5 to 6 ms lower (p50 257 to 258 ms on
the plain path, 278 ms with the added delay).

- **The plain path lost nothing** in three runs of 120 s: 86 960 packets sent
  and received each time, no retransmission.
- **With 1 % loss added in each direction, SRT repaired every loss in time.**
  About 900 of 87 000 packets were lost per run and resent; no window was
  incomplete or late, and nothing was dropped by SRT.
- **Sample age was steady:** p99 within about 1 ms of the median, the oldest
  sample 9 to 18 ms above it.
- No sample was altered. The sender's queue stayed far below its limit
  (high-water 18 to 73 records).
- CPU per 120 s run: about 6 s on each side.

## Against the criteria declared in advance

| # | Criterion | Result |
|---|---|---|
| 1 | Valid evidence, all five cases ran | **Met** |
| 2 | Every case: no altered sample; first index + received + reported missing = sent, END observed | **Met** in 5 of 5 |
| 3 | Clock bound at most 20 ms in every case | **Met**: 5.1 to 5.7 ms |
| 4 | Each `wan` case: at least 99.5 % usable, p99 age at most 400 ms | **Met** in 3 of 3: 100 %, 263 to 264 ms |
| 5 | Each `loss` case: the same limits | **Met** in 2 of 2: 100 %, 284 ms |

## Things to know about these numbers

- **The first few milliseconds of both `loss` cases were not delivered.** The
  receiver's first sample was number 552 and 690 (the first four and five
  records, about 6 ms of signal), reported as an initial join. The same is
  seen in emulation under loss. It lies inside the 10 s warm-up, so the
  figures above do not include it; the accounting does, and it adds up.
- **The clock.** The offset between the two machines' clocks was measured
  before and after every case from 64 two-way exchanges. Its error is at most
  half the shortest round trip (about 5 ms) plus half the change between the
  two measurements (up to 0.6 ms). That is a bound from the measurement
  itself, not a calibrated time reference. Against a 250 ms latency and a
  400 ms deadline it does not change any result here.
- **Added loss is still emulation.** The path itself lost nothing, so loss
  repair between two machines was shown with netem on both ends on top of the
  real delay, not with real network loss.
- **Not a strict run.** All cases ran segmented. Whether a strict stream
  survives the pauses of a shared machine, the question left open at 1 MS/s
  ([results-secondary-rep.md](results-secondary-rep.md)), was not asked here.
- A few UDP datagrams reached a port that had just closed on the sender
  machine (1 per plain case, 5 per loss case): late answers after a peer had
  ended. No receive-buffer or checksum error on either machine.

## What this changes

- The behaviour measured in emulation at 100 kS/s and 250 ms (complete,
  on time, age about latency plus path delay) was seen once between two
  machines on a real path with two clocks. The open item "two-host experiment
  with independent clocks, and a clock-offset procedure" is answered for this
  one path and this one configuration.
- It does not widen the supported envelope. Still not examined between two
  machines: the GNU Radio blocks, encryption, 1 MS/s, other latencies, the
  opposite direction, a path with real loss or congestion, a long run, and
  acquisition hardware.

It is not field qualification.
