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
