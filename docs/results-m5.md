# The blocks across emulated paths, and the release check (milestone M5)

## Read this first

- This is a functional check, not a measurement of timeliness. It asks one
  question: when the two GNU Radio blocks run encrypted over an impaired
  path, is every sample either delivered or reported? Age and on-time
  delivery were measured in M4 with the reference peers, in plaintext
  ([results-m4.md](results-m4.md)), and not again here.
- Same limits as M4: Linux netem in network namespaces, one virtualised
  aarch64 machine, sender and receiver on one host. No real network, no
  second host, no hardware.
- The check was written in this milestone and corrected twice after first
  results. Both corrections are described below. Three repetitions per
  path; this is not a statistical statement.

## What was run

`examples/headless_sender.py` and `examples/headless_receiver.py`, that is
`srt_sink` and `srt_source` in real flowgraphs: cf32 at 100 kS/s, latency
250 ms, AES-CTR with a random passphrase per run, `segmented_live` on both
ends, 120 s (12 000 000 samples) per run, receiver started first. Paths N0,
N1 and N2 as in M4. Command: `tools/dev/blocks-netem.sh OUT --seconds 120
--reps 3`. Summaries: [evidence/m5/](evidence/m5/).

## Result of the final series: 9 of 9 runs pass

| Path | Runs | Receiver joined at sample | Delivered | Reported missing, in tags | Unaccounted |
|---|---|---|---|---|---|
| N0 clean | 3 | 0 | 12 000 000 (all) | 0 | 0 |
| N1: 20 ms delay, 0.1 % loss | 3 | 4096 in each | 11 995 904 | 0 | 0 |
| N2: 40 ms delay, 1 % loss | 3 | 12 287 to 16 382 | 11 983 021 to 11 987 023 | 552 to 690 in 4 to 5 gaps | 0 |

In every run: joined-at plus delivered plus reported-missing equals the
number of samples sent; one discontinuity tag per break, each seen by the
consumer; no undecryptable packet; a 128-bit key in use; no failure on
either end.

## What it showed about the blocks

1. **Encryption under impairment works** as in plaintext: SRT retransmitted
   76 to 97 packets per run on N1 and 863 to 895 on N2, and the samples it
   could not recover in time were reported with their exact count.
2. **On a delayed path the first samples are not delivered.** A segmented
   Sink does not wait for the connection. While the handshake crosses the
   path, what it consumes expires after `queue_max_age_ms` (100 ms). The
   Source then starts at a later sample, tags it `initial_join`, and ends
   with `end: incomplete`. Lost at the start: 41 ms of signal on N1, 123 to
   164 ms on N2, and once 1.19 s on N2 when a handshake packet was lost.
   The Sink counts these samples in `tx_queue.dropped_expired_samples`. A
   strict Sink waits for its peer and does not do this.
3. **The END record can be lost, and then the receiver ends late.** END is
   a single packet with nothing after it. If it is lost, the receiver has
   no later packet that reveals the hole, so SRT does not ask for it again.
   The Sink waits 850 ms, sees it unacknowledged
   (`end_acknowledged_by_transport` false) and closes. The Source then
   waits its whole reconnect budget (60 s by default) for the sender to
   return and ends with `termination_unknown` true and failure
   `reconnect_timeout`. This happened in 1 of the 6 N2 runs of 120 s (and
   in none of 6 on N1). All samples had arrived in that run; only the end
   was unknown. The behaviour was already specified (D34); its frequency on
   a lossy path had not been observed before.

## Runs that failed, and corrections to the check

| Series | Outcome | What happened |
|---|---|---|
| Trial, 15 s per path | 2 of 3 failed | The check demanded delivered plus missing equals sent, and did not know about the late join (finding 2). The blocks had reported it; the check was wrong. Corrected; evidence of the trial not kept |
| First series, 3 × 3 × 120 s | 8 of 9 passed; `N2_rep1` failed `processes_ended` | END was lost (finding 3). The check waited 30 s for the receiver, which was correctly waiting out its 60 s reconnect budget, and stopped it. Kept as a failed run: [evidence/m5/blocks-netem-first/](evidence/m5/blocks-netem-first/). The check now waits for the budget and accepts an end that the Source declares unknown as a reported outcome |
| Final series, 3 × 3 × 120 s | 9 of 9 passed | The table above. END was not lost in any of these runs, so the corrected handling of a lost END was not exercised by a passing run |

## Release check

`tools/release/check.sh` builds the source package from one commit and then
executes every command block of [install.md](install.md), unchanged and in
order, as an unprivileged user in a fresh container of the pinned Ubuntu
24.04 image: system packages, Robotweax SRT at the pinned commit, the
module, a first encrypted transfer whose output must equal the text printed
in the guide, the two Companion examples, and the module's ten test suites.
It passes on aarch64 locally and runs on x86-64 in CI for every push.

## A newcomer trial, by an AI agent

A second AI agent that had been told nothing about the project received
only the source package and was asked to install it from its
documentation, run the examples, write its own sender and receiver for a
case the examples do not cover (f32, 48 kS/s, segmented, encrypted), and
provoke two typical mistakes.

- It installed the module, ran the examples and wrote a working pair of
  flowgraphs on the first attempt, bit-exact over 240 000 samples, without
  reading the implementation.
- It reported 18 documentation problems. The serious ones: the README did
  not lead to the install guide; a rate or format mismatch is reported on
  the Source only, and a wrong passphrase on the Caller only, which the
  documents did not say; most status fields were undocumented. All 18 were
  addressed in this milestone's documentation.

This is not the independent human reproduction the plan asks for. It is an
agent of the same kind as the author, on the same machine. That item stays
open.
