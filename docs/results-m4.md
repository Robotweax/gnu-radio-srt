# Network qualification: results of milestone M4

Measured on 2026-11-12 with the protocol in
[benchmarking.md](benchmarking.md), which was committed before these runs
(harness commit in `evidence/m4/harness-commit.txt`). Summaries of every
run, including the failed one, are in [evidence/m4/](evidence/m4/).

## Read this first

- **Emulation on one virtualised machine.** Three network namespaces with
  netem inside a container on Docker for Mac (aarch64, 16 CPUs, kernel
  7.0.14-linuxkit), four runs at a time, load average between 0.5 and 4.7.
  Not a reference host, not a real network, no second host, no hardware.
- **Reference peers, not the GNU Radio blocks.** The blocks share the
  worker, queue and codec measured here.
- **netem reorders heavily** when it applies jitter, far more than a real
  path with the same jitter figure. Every transport is affected, TCP most.
- **Loss patterns are not reproducible** run to run: this netem cannot be
  seeded.
- TCP used the kernel's default congestion control (`cubic`).
- Nothing here supports a claim about field use. It supports the statements
  under "Operating envelope" and nothing wider.

## Primary plan: cf32 at 100 kS/s, latency 250 ms

Five runs of 120 s per cell, 10 s warm-up, 11,000,000 offered samples per
run. Median (minimum–maximum) over the five runs. Deadline for "on time":
400 ms. "Usable" counts 1024-sample windows delivered completely and on time.

| Profile | Transport | Delivered % | On time % | Usable on time % | Age p50 ms | Age p99 ms |
|---|---|---:|---:|---:|---:|---:|
| N0 | SRT | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 254 (253–254) | 254 (254–256) |
| N0 | TCP | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 2 (2–2) | 3 (3–4) |
| N0 | UDP | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 253 (252–253) | 253 (253–253) |
| N1 | SRT | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 100.000 (100.000–100.000) | 278 (275–279) | 279 (276–280) |
| N1 | TCP | 100.000 (99.866–100.000) | 99.205 (96.560–100.000) | 99.143 (96.481–100.000) | 26 (26–26) | 388 (286–609) |
| N1 | UDP | 99.905 (99.885–99.925) | 99.905 (99.885–99.925) | 99.227 (99.069–99.358) | 273 (268–273) | 273 (268–274) |
| N2 | SRT | 99.996 (99.987–99.999) | 99.996 (99.987–99.999) | 99.963 (99.898–99.991) | 299 (299–303) | 302 (300–304) |
| N2 | TCP | 29.565 (28.548–32.426) | 0.000 (0.000–0.000) | 0.000 (0.000–0.000) | 1762 (1642–1901) | 3139 (2794–3621) |
| N2 | UDP | 99.015 (99.000–99.048) | 99.015 (99.000–99.048) | 91.965 (91.816–92.347) | 293 (285–302) | 294 (285–302) |

All 45 runs passed their mandatory gates.

**SRT gates in the declared envelope: 15 of 15 runs pass.**

- N0: exact, receiver-observed complete, in all five runs.
- p99 age stayed between 254 ms and 304 ms against a budget of 400 ms.
- N1: every sample delivered on time in all five runs.
- N2: between 138 and 1380 of 11,000,000 samples were not recovered
  within the latency budget. Every one of them was reported as missing.

### Comparison

The predeclared criterion for a use-case benefit was: on N1 or N2, SRT's
usable on-time fraction is higher than both baselines in all five runs.

- **N2 (40 ms, 10 ms jitter, 1 % loss each way): met.** SRT 99.90–99.99 %,
  UDP 91.8–92.3 %, TCP 0 %. UDP loses about 1 % of its records and each
  lost record spoils a 1024-sample window. TCP could not carry the stream
  at all: it delivered about 30 % of the samples, seconds late.
- **N1 (20 ms, 5 ms jitter, 0.1 % loss): not met against TCP.** SRT was at
  100 % in all five runs and above UDP (99.07–99.36 %) in all five. TCP
  reached 100 % in one run and 96.5–99.4 % in the others, so SRT was not
  higher in every run.

What the comparison also shows, and should not be left out:

- **SRT costs latency.** Its age is the configured latency plus the path
  delay: 254 ms on a clean path, where TCP delivered at 2 ms. On N1 TCP's
  median age was 26 ms against SRT's 278 ms.
- **On a mildly impaired path TCP is a serious alternative** when low
  latency matters more than a hard bound: on N1 it delivered everything in
  four of five runs, mostly within 30 ms, with a tail up to 609 ms.
- **SRT's advantage is the bounded tail under loss**, and it grows with
  loss and round-trip time: TCP's throughput collapses where SRT still
  recovers nearly everything inside the budget.
- Part of TCP's collapse on N2 comes from netem's reordering. The loss and
  round-trip time alone already put standard TCP below this stream's rate,
  so the direction of the result is not an artefact, but its size may be.

CPU time per 120 s run at 100 kS/s, median: SRT sender 3.5–4.1 s and
receiver 2.8–3.3 s; TCP sender 0.8–2.6 s and receiver 0.4–2.1 s. UDP
receiver CPU in these runs is not meaningful (see "Problems found").

## Secondary plan: one run per cell, exploratory

One run is not a comparison. These show behaviour and where to look next.

### Other profiles, 100 kS/s, 250 ms

| Profile | Transport | Delivered % | On time % | Usable on time % | Age p50 ms | Age p99 ms |
|---|---|---:|---:|---:|---:|---:|
| N3 | SRT | 99.753 | 99.753 | 99.069 | 304 | 305 |
| N3 | TCP | – | – | – | – | – |
| N3 | UDP | 93.030 | 93.030 | 83.717 | 295 | 295 |
| N4 | SRT | 99.606 | 83.715 | 83.707 | 280 | 5926 |
| N4 | TCP | 96.855 | 91.316 | 91.174 | 25 | 545 |
| N4 | UDP | 100.000 | 84.856 | 84.852 | 273 | 5135 |
| N5 | SRT | 95.437 | 95.437 | 95.429 | 274 | 275 |
| N5 | TCP | 93.232 | 92.832 | 92.822 | 23 | 24 |
| N5 | UDP | 95.449 | 95.449 | 95.438 | 273 | 273 |
| N6 | SRT | 100.000 | 100.000 | 100.000 | 277 | 278 |
| N6 | TCP | 87.143 | 39.838 | 39.587 | 471 | 990 |
| N6 | UDP | 99.896 | 99.896 | 99.106 | 269 | 269 |
| N3 | TCP | 12.540 | 0.000 | 0.000 | 2243 | 38311 |
| N4b | SRT | 95.679 | 95.679 | 91.239 | 278 | 315 |
| N4b | TCP | 97.110 | 91.391 | 91.351 | 25 | 806 |
| N4b | UDP | 96.850 | 96.850 | 91.044 | 271 | 273 |

- **N3 burst loss (about 6.5 % mean loss in bursts):** SRT kept 99.07 % of
  the windows usable, UDP 83.7 %. TCP's first run failed its gate (see
  below); the repeat delivered 12.5 % of the samples, none on time.
- **N4 constrained, deep bottleneck queue: SRT delivered late.** The
  forward rate was cut to 0.6 of the load for 10 s in front of a queue
  100000 packets deep. Nothing was lost, so nothing was retransmitted or
  dropped; the queue simply delayed everything. SRT delivered 99.6 % of the
  samples but only 83.7 % on time, with ages up to 6.3 s, and needed 9.0 s
  after the constraint ended to be on time again. UDP behaved the same.
  **The module does not bound end-to-end age when the network queues
  instead of dropping.** TCP did better here (91.2 % usable) because its
  sender felt the back-pressure and discarded stale records at the source.
- **N4b, same constraint with a queue of about 250 ms** (added afterwards):
  the excess was dropped in the network. SRT stayed on time (p99 315 ms)
  and all three transports kept about 91 % of the windows.
- **N5, 5 s outage:** SRT lost the 5 s (4.6 % of the window) and was on
  time again 0.37 s after the path returned, against a 10 s budget. UDP
  0.27 s, TCP 3.1 s with ages up to 8 s.
- **N6, asymmetric with 1 % loss on the return path:** SRT 100 %, UDP
  99.1 %, TCP 39.6 % usable.

### Latency setting, 100 kS/s

The deadline moves with the setting: 250 ms and 650 ms.

| Latency | Profile | Transport | Delivered % | On time % | Usable on time % | Age p50 ms | Age p99 ms |
|---|---|---|---:|---:|---:|---:|---:|
| 100 ms | N0 | SRT | 100.000 | 100.000 | 100.000 | 104 | 105 |
| 100 ms | N0 | UDP | 100.000 | 100.000 | 100.000 | 103 | 103 |
| 100 ms | N1 | SRT | 100.000 | 100.000 | 100.000 | 128 | 129 |
| 100 ms | N1 | UDP | 99.886 | 99.886 | 99.032 | 122 | 122 |
| 100 ms | N2 | SRT | 99.981 | 99.981 | 99.842 | 153 | 154 |
| 100 ms | N2 | UDP | 98.966 | 98.966 | 91.761 | 145 | 146 |
| 500 ms | N0 | SRT | 100.000 | 100.000 | 100.000 | 503 | 504 |
| 500 ms | N0 | UDP | 100.000 | 100.000 | 100.000 | 502 | 503 |
| 500 ms | N1 | SRT | 100.000 | 100.000 | 100.000 | 524 | 525 |
| 500 ms | N1 | UDP | 99.892 | 99.892 | 99.143 | 519 | 519 |
| 500 ms | N2 | SRT | 100.000 | 100.000 | 100.000 | 550 | 551 |
| 500 ms | N2 | UDP | 99.004 | 99.004 | 91.900 | 551 | 551 |

At 100 ms on N2 SRT still kept 99.84 % of the windows; at 500 ms it
recovered everything. One run each.

### 1 MS/s, 250 ms

| Profile | Transport | Delivered % | On time % | Usable on time % | Age p50 ms | Age p99 ms |
|---|---|---:|---:|---:|---:|---:|
| N0 | SRT | 100.000 | 100.000 | 100.000 | 253 | 254 |
| N0 | TCP | 100.000 | 100.000 | 100.000 | 1 | 2 |
| N0 | UDP | 100.000 | 100.000 | 100.000 | 251 | 251 |
| N1 | SRT | 100.000 | 100.000 | 100.000 | 277 | 278 |
| N1 | TCP | 20.287 | 0.000 | 0.000 | 2471 | 3728 |
| N1 | UDP | 99.905 | 99.905 | 99.205 | 270 | 270 |
| N2 | SRT | 99.997 | 99.997 | 99.970 | 302 | 303 |
| N2 | TCP | 3.304 | 0.000 | 0.000 | 14896 | 20889 |
| N2 | UDP | 98.991 | 98.991 | 91.831 | 290 | 290 |

SRT carried 1 MS/s (about 73 Mbit/s on the wire) through N0 to N2 with the
same behaviour as at 100 kS/s. CPU per 120 s run: sender 8.0–8.7 s,
receiver 5.1–5.4 s. One run each, on a machine with spare cores.

## Runs that failed or were repeated

| Run | Result | Cause |
|---|---|---|
| `N3_tcp_r100000_l250_rep0` | failed `processes_ended` | The TCP connection stalled under burst loss. The sender was still waiting to hand over END when the harness's limit expired. The wait included SRT's 60 s reconnect budget, which makes no sense for TCP. Kept as a failed run. |
| `N3_tcp_r100000_l250_rep1` | pass | Repeat after limiting that wait for the baselines. TCP delivered 12.5 %, none on time. |

No SRT run failed or was repeated.

## Problems found by the measurement

1. **Late delivery under queueing congestion (N4).** A real limitation of
   the module, described above. A receiver can detect it only if the
   stream carries capture times and the clocks are related. Not fixed.
2. **UDP comparator spun a CPU core** in its playout loop during the
   primary and secondary plans (receiver CPU 33–40 s per 120 s run). It did
   not change what UDP delivered, but UDP CPU figures from those runs are
   invalid. Fixed before the follow-up run (7.5 s).
3. **TCP comparator waited too long at the end** (the failed run above).
   Fixed before the follow-up run.
4. Earlier, in harness checks before the protocol was committed: a
   window-boundary counting error, and a TCP comparator whose default
   kernel buffer delivered 25-second-old data. Both fixed before any
   headline run.

## Operating envelope supported by this evidence

Stated as narrowly as the evidence allows.

**Shown, with five runs per cell, in emulation on one machine:** cf32 at
100 kS/s with 250 ms latency over paths up to 40 ms delay, 10 ms jitter and
1 % random loss in each direction, with a bottleneck of three times the
load: exact on a clean path; under impairment at least 99.898 % of
1024-sample windows complete and on time, p99 age at most 304 ms, and
every missing sample reported.

**Seen once each, not yet repeated:** 1 MS/s on the same profiles; 100 ms
and 500 ms latency; burst loss; asymmetric loss; recovery 0.37 s after a
5 s outage.

**Known not to hold:** timeliness when a bottleneck queues for seconds
instead of dropping.

**Not examined at all:** a real network, two hosts with separate clocks,
acquisition hardware, a dedicated host, x86-64 for the full matrix, rates
above 1 MS/s, f32 under impairment, encryption under impairment, the GNU
Radio blocks under netem, congestion with competing traffic.

**Use-case benefit:** shown against both baselines at 1 % loss (N2). Not
shown against TCP at 0.1 % loss (N1), where TCP also delivers and does so
with a tenth of the latency.
