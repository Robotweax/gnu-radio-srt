# Known limitations of 0.1.0-rc2

Read this before relying on the module for a measurement.

## What has not been tried

- **A real network.** All network results come from Linux netem in network
  namespaces on one machine. netem reorders packets more than a real path
  with the same jitter figure.
- **Two hosts, beyond one run.** One run of the reference peers between two
  cloud machines on one path met its criteria
  ([results-two-host-1.md](results-two-host-1.md)). The GNU Radio blocks,
  encryption, 1 MS/s, a path with real loss and a long run were not run
  between two machines.
- **Acquisition hardware.** The Sink was fed by synthetic, throttled
  sources. No SDR front end, no hardware timestamps, no overrun reports.
- **A dedicated machine.** Timing figures come from a shared, virtualised
  host.
- **Rates above 1 MS/s**, and `f32` under impairment. 1 MS/s itself is
  not a supported rate: it failed one of 15 emulated runs, when a 76 ms
  pause of the receiving process ended a strict run
  ([results-secondary-rep.md](results-secondary-rep.md)).
- **An independent user.** Nobody outside the project has installed or used
  it. The install guide is executed in a fresh container by a script, which
  checks the commands and not whether a person understands them.

Field readiness is not claimed.

## Behaviour to know about

- **By default, age is not bounded when the network queues instead of
  dropping.** If a bottleneck carries less than the stream for seconds in
  front of a deep queue, nothing is lost, so nothing is retransmitted or
  dropped, and samples arrive seconds old (6.3 s in one emulated case).
  `latency_ms` bounds delay only while the path keeps up. Since 0.1.0-rc2
  the Source can discard such records (`max_lateness_ms`, off by default);
  the samples are then missing and reported instead of late.
- **SRT adds its latency to every sample.** With the default of 250 ms the
  measured age was 254 to 304 ms. On a path with little loss TCP delivered
  with a tenth of that. The benefit over TCP and UDP was shown at 1 % loss
  and was not shown at 0.1 % loss.
- **A segmented Sink does not wait for its peer.** Samples it consumes
  before the connection is up, or during an outage, are discarded after
  `queue_max_age_ms`. The Source then joins the run later and marks its
  first sample with an `initial_join` tag, counts no missing samples for
  the part it never saw, and reports `end` as `incomplete` at the end. On
  loopback with the receiver started first nothing is lost; on a path with
  40 ms delay the first 0.1 s typically is. Use `strict_stop` on the Sink
  if the stream must start at its first sample.
- **A Sink learns about the receiver only if the receiver tells it.**
  Since 0.1.0-rc2 a Source that ends a run because of what it received
  (wrong rate or format, a loss in strict mode) says so, and the Sink fails
  with the same reason. That message can be lost, comes too late for a run
  shorter than about two latencies, and is neither sent nor read by
  0.1.0-rc1. A Listener still does not notice a Caller with a wrong
  passphrase.
- **A strict Sink cannot see network loss by itself.** Only the Source
  can. Run the Source in strict mode too if any loss must end the run; it
  then tells the Sink.
- **An abrupt end can lose the last latency window** without the Source
  being able to tell how much. It reports `termination_unknown`.
- **Downstream blocks must honour the tags.** In segmented mode samples
  before and after a `rwsrt.discontinuity` tag are not contiguous. A block
  that ignores tags will process across the gap. `examples/gap_aware_spectrum.py`
  shows the handling.
- **Capture times are only as good as their anchor.** The module carries
  capture-time tags and their uncertainty. It does not synchronise clocks,
  measure offsets or correct drift.
- **Synthetic sources must be throttled** to the declared sample rate.

## Security boundary

- AES-CTR protects confidentiality. **It authenticates neither the payload
  nor the peer.** Anyone who knows the passphrase can connect; anyone on the
  path can alter ciphertext without detection by the cipher. Header
  validation is not integrity protection.
- The passphrase comes from an environment variable. That is a development
  convenience, not secret management: other processes of the same user can
  usually read it.
- The default bind address is loopback. Binding to another address exposes
  a UDP port; a Listener accepts the first peer that presents the right
  `stream_id` and passphrase.
- Authenticated encryption (AES-GCM) is not offered.

## Not implemented

Vector ports, more than one stream per block, sample formats other than
`f32` and `cf32`, DNS names, IPv6, rendezvous, SRT file mode, FEC, bonding,
automatic resampling or clock recovery, interpretation of UHD-style
`rx_time` tags, binary packages.

## Where the evidence is

| Topic | Document |
|---|---|
| Network measurements and operating envelope | [results-m4.md](results-m4.md) |
| How they were measured | [benchmarking.md](benchmarking.md) |
| The blocks, encrypted, across emulated paths | [results-m5.md](results-m5.md) |
| The lateness bound, repeated END and receiver notice | [results-rc2.md](results-rc2.md) |
| Every finding and decision | [decisions.md](decisions.md) |
