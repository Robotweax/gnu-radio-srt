# Network qualification: protocol

How `gr-robotweax-srt` is measured under network impairment, and what would
count as a pass. **This file was committed before the first headline run.**
Any later change to a definition, a gate or a profile is listed under
"Changes after the first results" at the end, with its reason. Results are
in [results-m4.md](results-m4.md) once they exist.

Scope: transport behaviour of the RWSR records over SRT, compared with UDP
and TCP carrying the same records. The GNU Radio blocks use the same worker,
queue and codec as the reference peers measured here; the blocks themselves
are covered functionally by the QA suites, not by this protocol.

## What these measurements can and cannot show

- They run in network namespaces inside one container on a shared,
  virtualised machine (Docker on an Apple Silicon Mac, or a hosted CI
  runner). That is good for behaviour and for coarse comparisons. **It is
  not a reference host.** Latency figures carry scheduling noise of the
  host and are indicative only.
- One host means one clock. Capture-to-output age is measured exactly
  because sender and receiver share `CLOCK_MONOTONIC`.
- There is no two-host experiment and no hardware acquisition here. Field
  readiness is not claimed and cannot be concluded from this protocol.
- netem's random generator cannot be seeded with the iproute2 in the image
  (6.1). Loss realisations differ between runs; repetitions are the
  safeguard.
- netem applies jitter per packet, which reorders packets heavily at these
  packet rates. Real paths with the same jitter figure usually reorder far
  less. This stresses every transport and TCP most.

## Topology

`tests/netem/topology.sh` creates three network namespaces per run: sender,
router, receiver, joined by two veth pairs. netem sits in the router only:
on the interface toward the receiver (forward direction) and on the
interface toward the sender (reverse direction). Everything is created
inside new namespaces and removed with them; the host's routes and qdiscs
are never touched. The runner records the qdisc configuration, the qdisc
statistics after the run, and a 20-packet ping before it.

## Profiles

From plan section 12 C, as implemented in `tests/netem/profiles.json`.
"x3" is three times the offered wire load, so the bottleneck is at least
twice the load as the plan requires. The emulator's queue is 100000
packets deep so that the profile, not the queue, decides what is lost.

| Profile | Delay, each way | Jitter | Loss | Bottleneck / event |
|---|---|---|---|---|
| N0 clean | none | none | none | none |
| N1 modest WAN | 20 ms | 5 ms | 0.1 % each way | x3 |
| N2 impaired WAN | 40 ms | 10 ms | 1 % each way | x3 |
| N3 burst loss | 40 ms | 10 ms | Gilbert-Elliott, each way: 1 % to enter the bad state, 10 % to leave, 70 % loss while bad, 0.1 % while good (mean burst about 10 packets, mean loss about 6.5 %) | x3 |
| N4 constrained | 20 ms | 5 ms | none | x3; forward rate cut to 0.6 of the load for 10 s |
| N4b (added later) | 20 ms | 5 ms | none | as N4, with a bottleneck queue of about 250 ms |
| N5 outage | 20 ms | none | none | x3; both directions dropped for 5 s |
| N6 asymmetric | 20 ms forward, 80 ms reverse | 5 ms | 0.1 % forward, 1 % reverse | x3 |

Events (N4, N5) start 40 s into the measurement window.

These are experiment conditions. None of them is a claim that the module
works under them.

## Transports and fairness

All three use the same sender and receiver programs
(`tools/reference_peer`), the same test signal, the same RWSR framing and
1200-byte records, the same bounded queues with the same limits, the same
continuity ledger and the same analysis. Only the transport differs.

| Transport | What it is |
|---|---|
| `srt` | The module's session worker over Robotweax SRT, requested latency as configured |
| `udp` | One record per datagram, no retransmission. The receiver delays every record by the same latency (first arrival plus latency plus nominal sample offset) and releases records in order |
| `tcp` | One connection, `TCP_NODELAY`, send buffer of one latency budget of data. Records are released as they arrive |

Choices made so that no baseline is a strawman:

- UDP gets the same latency budget as SRT for reordering. Without it, it
  could not survive netem's reordering at all.
- TCP's send buffer is limited to one latency budget. With the kernel
  default it parked seconds of stale samples (seen in a harness check).
  With the limit, stale records are discarded by the same age-limited queue
  that the other transports use.
- All transports run in plaintext, so encryption cost is not charged to
  SRT alone.
- SRT bandwidth settings are the module's defaults (D20). No transport is
  tuned per profile.

Continuity policy: strict on N0 (any loss fails the run), segmented on all
other profiles (the stream continues and gaps are counted).

The sender is paced by a sample clock: it produces what is due every
millisecond. An unpaced generator would measure throughput, not a
real-time stream.

## Metrics

Sample k has the nominal capture time `t0 + k / rate` on the shared
monotonic clock, where `t0` is when the sender's sample clock started. The
receiver logs the output time of every record.

- **Measurement window:** samples from 10 s after `t0` (warm-up) to the end
  of the run. Offered samples are those in the window.
- **Age** of a record: output time minus the nominal capture time of its
  first (oldest) sample. Percentiles are weighted by samples.
- **Delivered fraction:** delivered samples in the window / offered.
- **Deadline:** requested latency + 150 ms, the same for every transport.
- **On-time fraction:** samples delivered with age within the deadline /
  offered.
- **Usable on-time fraction:** the window is cut into analysis windows of
  1024 consecutive samples. A window is usable if every one of its samples
  was delivered on time. The fraction is usable windows / all windows. This
  is the figure that matters for block processing such as an FFT.
- **Missing samples:** offered minus delivered.
- **Recovery time** (N5): time from the restoration of the path to the
  first record that is output on time.
- CPU seconds and peak resident memory of each peer; queue high-water marks.

## Gates

Derived from the recorded evidence by `tools/qual_common.py`. A missing
file or a hung process is a failure, never a pass. Results are pass, fail
or inconclusive.

| Gate | Applies to | Mandatory for |
|---|---|---|
| Evidence present, processes ended | every run | every run |
| No altered sample | every run | every run |
| Samples accounted: first index + received + reported missing = sent, END observed | every run | every run where END was observed; inconclusive without END |
| N0: zero missing in the window, exact count, receiver-observed complete | N0 | SRT |
| p99 age within effective latency + 150 ms | SRT on N0, N1, N2 | SRT in the primary envelope |
| Queues bounded | N4, N5 | every transport |
| Recovery within 10 s | N5 | SRT |

Delivered, on-time and usable fractions on N1 to N6 are **reported, not
gated**. No loss-recovery threshold is invented. UDP and TCP are
comparators: they carry only the integrity and accounting gates.

## Plans

**Primary (headline).** The declared operating envelope: cf32 at
100 kS/s, requested latency 250 ms, profiles N0, N1, N2, all three
transports, five repetitions of 120 s each, 10 s warm-up. The transport
order rotates with the repetition.

A use-case benefit would be: on at least one of N1 or N2, SRT's usable
on-time fraction is higher than that of both baselines in all five
repetitions. If that is not observed, the software is still correct but no
benefit is claimed.

**Secondary (exploratory, one run each).** Not headline, not enough runs
for a comparison:

- N3 to N6 at 100 kS/s and 250 ms, all transports.
- Latency 100 ms and 500 ms on N0 to N2, SRT and UDP.
- 1 MS/s at 250 ms on N0 to N2, all transports.

Four runs execute at a time, each in its own namespaces. Load averages are
recorded per run. Parallel runs on one machine can disturb each other's
timing; the primary plan is the same for every transport, so the
disturbance is not systematic.

Every run is kept, including failed and inconclusive ones.

## Reproducing

```sh
tools/dev/build-image.sh
tools/dev/netem.sh primary artifacts/qual/primary --parallel 4
tools/dev/netem.sh secondary artifacts/qual/secondary --parallel 4
python3 tools/validate_results.py artifacts/qual/primary --summary-dir docs/evidence/m4/primary
```

Each run directory holds `manifest.json`, `sender.jsonl`, `receiver.jsonl`,
`network.json`, `metrics.json` and `verdict.json`. The validator derives
every verdict again from that evidence and exits nonzero if a mandatory
gate failed.

## Changes after the first results

First results: 2026-11-12, primary and secondary plans. No definition,
gate, deadline or existing profile was changed. Three things were added or
corrected afterwards, and one follow-up plan was run:

1. **UDP comparator, CPU only.** Its playout loop polled without waiting
   and used a full CPU core. Delivery and timing figures were not
   affected; UDP CPU figures of the primary and secondary plans are
   invalid. Corrected before the follow-up plan.
2. **TCP and UDP comparators, end of run.** After the last sample the
   sender waited for END to be handed over for up to 66 s, because the
   wait included SRT's reconnect budget. On N3 the TCP connection had
   stalled, the harness's 60 s limit expired first, and the run failed its
   `processes_ended` gate. The failed run is kept. The wait is now 6 s for
   the baselines.
3. **Profile N4b added:** N4 with a bottleneck queue of about 250 ms of
   traffic at the constrained rate, instead of 100000 packets. N4 itself
   is unchanged. Reason: N4 showed that the deep queue, not the transport,
   decided the outcome.

Follow-up plan (`--plan followup`), one run each: N3 over TCP again, N4b
over all three transports, and N2 over UDP to check the corrected CPU use.

### After 0.1.0-rc1: plan `rc2` (written 2026-11-13, before its runs)

The Source gained an optional bound, `max_lateness_ms`: a record that SRT
hands over later than that after its scheduled delivery time is discarded
and reported as a gap. It answers the limitation that N4 exposed. The
software also now sends END three times and lets a receiver tell the
sender why it ended a run; neither changes what this protocol measures.
No existing definition, gate, profile or plan was changed.

Plan `rc2`, SRT only (the baselines have no such bound), cf32 at 100 kS/s,
latency 250 ms, 120 s per run with 10 s warm-up, three repetitions:

| Cell | Purpose |
|---|---|
| N4, no bound | Reference: repeats the M4 observation with this build |
| N4, bound 150 ms | Does the bound limit the age of what is delivered |
| N2, bound 150 ms | Cost of the bound on a lossy path that keeps up |
| N0, bound 150 ms | Cost of the bound on a clean path |

Criteria, fixed before the runs. They are evaluated from `metrics.json`
and the receiver's final status; the existing gates still apply to every
run as before.

1. **N4 with the bound:** the oldest delivered sample in the measurement
   window is at most 525 ms old (latency 250 + bound 150 + path delay and
   jitter 25 + 100 margin for scheduling on the shared host).
2. **N4 with the bound:** every sample is delivered or reported: missing
   samples equal the samples discarded as late plus those SRT itself did
   not deliver, and no break of unknown size occurs.
3. **N0 with the bound:** no record is discarded as late.
4. **N2 with the bound:** at most 0.01 % of the samples are discarded as
   late.
5. **N4 without the bound** is expected to show ages of seconds again. If
   it does not, the comparison says nothing and that is reported.

What a pass would and would not mean: the bound limits age in this
emulated case of a deep queue. It does not make more samples arrive on
time; what was late is lost either way. It says nothing about a real
network.

### Plan `secondary-rep` (written 2026-11-14, before its runs)

Two groups of the secondary plan had one run per cell and results that
looked like a difference between the transports: burst loss (N3) at
100 kS/s, and 1 MS/s on N0 to N2. One run is not a comparison. This plan
repeats them so that they can be read like the primary plan. No existing
definition, gate, deadline, profile or plan was changed. The build is the
one tagged `v0.1.0-rc2`, with `max_lateness_ms` off.

Plan `secondary-rep`: all three transports, requested latency 250 ms,
120 s per run with 10 s warm-up, five repetitions, transport order
rotating with the repetition, four runs at a time:

| Cell | Runs |
|---|---|
| N3 at 100 kS/s | 15 |
| N0, N1, N2 at 1 MS/s | 45 |

Criteria, fixed before the runs. They use the metrics and gates defined
above.

1. **Burst loss.** A benefit of SRT on N3 is stated only if its usable
   on-time fraction is higher than that of both baselines in all five
   repetitions. Otherwise none is stated.
2. **1 MS/s, loss.** The same rule, separately for N1 and for N2.
3. **1 MS/s, envelope.** 1 MS/s on N0 to N2 is added to the supported
   envelope of this evidence only if all 15 SRT runs pass every gate that
   applies to them, including zero missing samples on N0 and p99 age
   within effective latency + 150 ms. If any fails, 1 MS/s stays
   exploratory and the failure is reported.
4. TCP and UDP carry only the integrity and accounting gates, as before.
   A baseline run that fails a gate is kept and reported; it does not
   decide criteria 1 to 3 in SRT's favour by being absent: a repetition
   with a missing baseline figure counts as not higher.

What a pass would and would not mean: a repeated difference in this
emulation on one machine. Four runs at 1 MS/s share the host; load
averages are recorded. It says nothing about a real network or about
rates above 1 MS/s.

### Plan `two-host-1` (written 2026-11-14, before its run)

Everything above ran on one machine with one clock. This plan is the first
run between two machines over a real path. No existing definition, gate,
deadline, profile or plan is changed.

**Setup.** Two cloud virtual machines of one provider in two German regions
(sender in Frankfurt, receiver in Berlin), Ubuntu 24.04 x86-64, 2 vCPU and
4 GiB each, public IPv4, shared hosts. A private test harness of the
maintainer creates them, builds on both and removes them; it is not part of
this repository. Both machines build Robotweax SRT `e45d0edd` (0.3.2) and the
reference sender and receiver from the published 0.1.0-rc2 source (commit
`c25c678`, content hash `bbbe05e1…4210`), and the two builds must be
identical. The GNU Radio blocks are not part of this run.

**Cases**, in this order, one run each, 120 s, cf32 at 100 kS/s, latency
250 ms, segmented continuity, plaintext, `max_lateness_ms` off:

| Case | Path |
|---|---|
| `wan-1`, `wan-2`, `wan-3` | the path between the two regions as it is |
| `loss-1`, `loss-2` | the same path plus netem on both machines' egress towards the other: 20 ms delay, 1 % random loss |

Order: `wan-1`, `loss-1`, `wan-2`, `loss-2`, `wan-3`.

**Clocks.** The two machines' monotonic clocks are unrelated. Before and
after every case, outside netem, 64 two-way UDP exchanges are recorded. The
offset comes from the exchange with the shortest round trip; its error is at
most half that round trip, plus half the difference between the two
measurements of a case. Ages below are taken **at that bound**: every sample
is counted as old as the clock error allows.

**Metrics** are those defined above (10 s warm-up, deadline 400 ms, windows
of 1024 samples), computed by the harness's validator from the receiver's
per-record log.

**Criteria**, fixed before the run:

1. **Complete.** The harness reports valid evidence and all five cases ran.
   If not, the result is reported as incomplete and criteria 2 to 5 are not
   evaluated for the missing part.
2. **Integrity and accounting,** every case: no altered sample; first index
   + received + reported missing = sent, with END observed.
3. **Clock.** The clock bound of every case is at most 20 ms. For a case
   above that, ages are reported as not measurable and criteria 4 and 5 are
   not evaluated for it.
4. **Real path,** each of the three `wan` cases: at least 99.5 % of the
   windows complete and on time, and p99 age at most 400 ms.
5. **Real path with added loss,** each of the two `loss` cases: the same two
   limits.

Where the limits come from: in emulation the supported envelope gave at
least 99.898 % of the windows and a p99 age of at most 304 ms on paths up to
40 ms delay and 1 % loss. 99.5 % and the unchanged 400 ms deadline leave room
for shared hosts: one pause of 100 ms costs about 0.09 % of a run.

**One run.** A failed or incomplete run is reported as it is. It is not
repeated to obtain a better result; a later run would be a new plan.

What a pass would and would not mean: the behaviour seen in emulation was
also seen once between two machines on one real path with two clocks. One
direction, one day, shared virtual machines, the reference peers and not the
blocks, no encryption, one rate. Netem on top of a real path is still
emulated loss. It is not field qualification.

