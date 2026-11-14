# Repetitions of two secondary cases: burst loss and 1 MS/s

## Read this first

- Same limits as before: Linux netem in network namespaces on one
  virtualised aarch64 machine, sender and receiver on one host, reference
  sender and receiver. No real network, no second host, no hardware.
- Plan `secondary-rep` and its criteria were written into
  [benchmarking.md](benchmarking.md) and committed (`a1f1ef6`) before the
  runs. No trial run preceded it. Build: the code tagged `v0.1.0-rc2`,
  `max_lateness_ms` off.
- 60 runs, five per cell, 120 s each, four at a time. 59 passed their
  gates. **One SRT run failed** and is kept; it decides criterion 3.

## Results

Median (minimum to maximum) over five runs. Latency 250 ms, deadline
400 ms.

| Profile | Rate | Transport | Delivered % | Usable on time % | Age p99 ms | Pass/fail |
|---|---|---|---|---|---|---|
| N3 burst loss | 100 kS/s | SRT | 99.713 (99.681 to 99.760) | 98.967 (98.855 to 99.060) | 302 (299 to 305) | 5/0 |
| N3 | 100 kS/s | UDP | 93.603 (93.429 to 93.984) | 84.703 (84.257 to 85.737) | 303 (288 to 304) | 5/0 |
| N3 | 100 kS/s | TCP | 3.745 (0.477 to 5.778) | 0 | 83828 (33300 to 111470) | 5/0 |
| N0 clean | 1 MS/s | SRT | 100 (50.774 to 100) | 100 (50.774 to 100) | 253 (253 to 254) | **4/1** |
| N0 | 1 MS/s | UDP | 100 | 100 | 253 (252 to 253) | 5/0 |
| N0 | 1 MS/s | TCP | 100 | 100 | 2 (1 to 2) | 5/0 |
| N1 0.1 % loss | 1 MS/s | SRT | 100 (99.950 to 100) | 100 (99.939 to 100) | 279 (276 to 279) | 5/0 |
| N1 | 1 MS/s | UDP | 99.905 (99.897 to 99.907) | 99.195 (99.144 to 99.216) | 277 (268 to 277) | 5/0 |
| N1 | 1 MS/s | TCP | 20.341 (19.565 to 23.177) | 0 | 3872 (3378 to 5043) | 5/0 |
| N2 1 % loss | 1 MS/s | SRT | 99.997 (99.995 to 99.998) | 99.971 (99.958 to 99.980) | 300 (297 to 304) | 5/0 |
| N2 | 1 MS/s | UDP | 99.003 (98.988 to 99.019) | 91.903 (91.776 to 92.056) | 296 (285 to 301) | 5/0 |
| N2 | 1 MS/s | TCP | 3.161 (2.902 to 3.563) | 0 | 20094 (18578 to 21040) | 5/0 |

The baselines carry only the integrity and accounting gates, so a TCP run
that delivers 3 % of the samples, all late, still passes them.

## Against the criteria declared in advance

| # | Criterion | Result |
|---|---|---|
| 1 | N3: SRT's usable on-time fraction above both baselines in all five repetitions | **Met**, 5 of 5. Lowest SRT 98.855 %, highest UDP 85.737 %, TCP 0 % |
| 2 | 1 MS/s: the same on N1, and on N2 | **Met** on both, 5 of 5 each. N1: lowest SRT 99.939 %, highest UDP 99.216 %. N2: lowest SRT 99.958 %, highest UDP 92.056 % |
| 3 | 1 MS/s joins the supported envelope only if all 15 SRT runs pass every gate | **Not met.** 14 of 15. 1 MS/s stays exploratory |
| 4 | Baseline runs kept and counted | All 30 present; none missing |

## The failed run

`N0_srt_r1000000_l250_rep2`: SRT, clean path, 1 MS/s, strict continuity
(the N0 gate demands an exact stream). After 66 s the run ended with
`queue_overflow`, "queued record outlived queue_max_age", at the receiver.
50.8 % of the window had been delivered, all of it exact and on time.

What the evidence shows:

- The receiver's network thread was not scheduled for 76 ms once
  (`max_wait_slice_ms` 76.4; 22 to 28 ms in the other runs). SRT then
  handed over the backlog at once, 543 records stood in the receiver's
  queue, and one of them waited longer than the 100 ms the reference
  receiver allows (`queue_max_age`). In strict mode that ends the run.
- Nothing was lost on the network: no loss, no retransmission, no drop in
  SRT's statistics.
- The UDP run that executed at the same time shows the same disturbance:
  queue high-water marks of 475 and 859 records against 10 to 200 in its
  other runs, oldest sample 370 ms. It has no strict mode and went on.
- A second SRT run, `N1_srt_r1000000_l250_rep2`, had a wait of 110 ms. It
  ran segmented: 55 338 samples (0.05 % of the window) were dropped from
  the receiver's queue as too old and reported as four gaps of exact
  size. It passed its gates.
- Both ends reported the failure with its reason. The sender showed
  "reported by the peer: queued record outlived queue_max_age": the
  receiver notice added in rc2, seen here outside a test.

What it does not show: whether the pause comes from the shared host (four
runs at 1 MS/s on one virtual machine) or would also occur on a machine of
its own. The simultaneous disturbance of the UDP run points to the host.
That is not proof, and the criterion was fixed in advance, so it stands as
not met.

What follows for a user: at 1 MS/s a pause of about 100 ms in the
receiving process is 7 000 records. With `queue_max_age_ms` at 100 a
strict Source ends the run on such a pause and a segmented one reports a
gap. Neither is silent. On a host that can pause that long, a strict
stream at this rate needs a larger `queue_max_age_ms` and queue, at the
price of older samples; this was not measured.

## What this changes

- **Burst loss at 100 kS/s:** the single M4 run is confirmed. A benefit of
  SRT over both baselines is now stated for N3 as it is for N2, in this
  emulation.
- **1 MS/s with loss:** a benefit over both baselines is stated for N1
  and N2. Note the difference from 100 kS/s, where TCP kept up on N1: at
  1 MS/s TCP did not.
- **1 MS/s as a supported rate: no.** It worked in 14 of 15 runs and
  failed once in a way that was reported. It stays outside the supported
  envelope until it is repeated on a host of its own.
- CPU per 120 s SRT run at 1 MS/s: sender 6.0 to 12.7 s, receiver 3.1 to
  6.1 s.

Evidence: [evidence/m4/secondary-rep/](evidence/m4/secondary-rep/)
(summary of all 60 runs, and the records of the failed run).
