# Block reference

Two blocks: **SRT Sink** (`robotweax_srt.srt_sink`) sends one sample stream,
**SRT Source** (`robotweax_srt.srt_source`) receives it. Both ends must run
this module; an ordinary SRT media player does not understand the payload
([protocol-v1.md](protocol-v1.md)). Tags and time are described in
[continuity-and-time.md](continuity-and-time.md).

Status of this document: it describes what is implemented and tested on
loopback, including with a fault-injecting relay. What was measured on
emulated network paths is in [results-m4.md](results-m4.md) and
[results-m5.md](results-m5.md); what was never tried is in
[limitations.md](limitations.md).

## Construction

```python
from gnuradio import robotweax_srt

transport = robotweax_srt.make_transport_config(
    role="caller", host="192.0.2.10", port=9000, passphrase_env="RWSRT_PASSPHRASE")
stream = robotweax_srt.make_stream_config(format="cf32", sample_rate_num=100000)
sink = robotweax_srt.srt_sink(transport, stream)
```

A complete receiving flowgraph for real-valued samples:

```python
from gnuradio import blocks, gr, robotweax_srt

transport = robotweax_srt.make_transport_config(
    role="listener", bind_address="127.0.0.1", port=9000,
    passphrase_env="RWSRT_PASSPHRASE")
stream = robotweax_srt.make_stream_config(
    format="f32", sample_rate_num=48000, continuity="segmented_live")
tb = gr.top_block()
source = robotweax_srt.srt_source(transport, stream)
tb.connect(source, blocks.file_sink(gr.sizeof_float, "received.f32"))
tb.run()                          # returns when the Source finishes
print(source.status()["session"]["end"], source.status()["session"]["failure"])
```

- `format` decides the item type of the port: connect `f32` to `float`
  ports and `cf32` to `complex` ports. GNU Radio refuses to connect a
  mismatch.
- A Caller needs `host`; a Listener needs none and uses `bind_address`.
- `format`, the sample rate, `stream_id` and the passphrase must be equal
  on both ends. `continuity` is chosen for each end separately.

`make_transport_config` and `make_stream_config` reject unknown names and
unknown enumeration values. Every value is validated when the block is
constructed; an invalid one raises before any thread or socket exists. All
values are fixed while the block runs.

In C++: `srt_sink::make(const transport_config&, const stream_config&)` and
the same for `srt_source`, from `<gnuradio/robotweax_srt/srt_sink.h>`.

## Ports

| Block | Stream | Message |
|---|---|---|
| SRT Sink | one input, `float` (f32) or `complex` (cf32) | output `status` |
| SRT Source | one output, same types | output `status` |

There is no command input. Neither block forwards tags as they are.

## Transport parameters

| Parameter | Default | Meaning |
|---|---|---|
| `role` | `caller` | `caller` or `listener`. Independent of sample direction |
| `host` | none | Caller: numeric IPv4 address of the peer |
| `bind_address` | `127.0.0.1` | Listener: local address. Change it explicitly for remote access |
| `port` | 9000 | 1 to 65535 |
| `stream_id` | `rwsrt/v1/default` | Routing label. A Listener closes peers with a different one. Not an identity |
| `latency_ms` | 250 | Requested SRT receiver and peer latency |
| `connect_timeout_ms` | 3000 | Per connection attempt |
| `startup_timeout_ms` | 30000 | No peer by then: terminal failure `startup_timeout` |
| `peer_idle_timeout_ms` | 5000 | Silence after which the connection counts as lost. At least 1000 |
| `reconnect` | true | Used in segmented mode only. Strict mode never reconnects |
| `reconnect_initial_ms`, `reconnect_max_ms` | 250, 4000 | Exponential retry delay with 20 % jitter |
| `reconnect_total_timeout_ms` | 60000 | Budget after an established connection failed |
| `stop_timeout_ms` | 1000 | Bound for handing END to SRT at a natural end |
| `max_lateness_ms` | 0 | Source only. Discard a record that SRT hands over later than this after its scheduled time, and report the gap. 0 keeps everything. See "Bounding the age" |
| `security` | `ctr` | `ctr` or `none`. Never changed automatically |
| `passphrase_env` | none | Name of the environment variable holding the passphrase |
| `status_interval_ms` | 1000 | Period of status messages |

## Stream parameters

| Parameter | Default | Meaning |
|---|---|---|
| `format` | `cf32` | `f32` or `cf32`. Must match on both ends |
| `sample_rate_num`, `sample_rate_den` | 100000, 1 | Rational samples per second. Must match on both ends |
| `record_limit_bytes` | 1200 | Whole record including the 96-byte header. 104 to 1456 |
| `flush_interval_ms` | 2 | Sink: longest time a partial record waits |
| `queue_max_bytes` | 4 MiB | Local record queue, serialized bytes |
| `queue_max_records` | 8192 | Local record queue, records |
| `queue_max_age_ms` | 100 | Longest time a record may wait in the local queue |
| `continuity` | `strict_stop` | `strict_stop` or `segmented_live` |

A 1200-byte record holds 138 cf32 or 276 f32 samples.

## Continuity policies

**`strict_stop`** (default). Any loss of continuity is a terminal failure:
a full or aged local queue, a sample gap, an unknown upstream gap, a
connection loss, a receiver joining mid-run. The Sink consumes nothing until
a peer is connected, which pushes back on upstream. Use it when downstream
processing ignores tags.

**`segmented_live`**. The stream keeps running. Old records are discarded
from full or aged queues, connections are re-established, and every break is
reported on the Source's output as a `rwsrt.discontinuity` tag. Use it only
with a downstream chain that honours those tags.

## Encryption

`security="ctr"` uses standard SRT AES-CTR with enforced encryption. It
protects confidentiality. **It does not authenticate the payload or the
peer.** Field validation and successful parsing are not integrity
protection.

- The passphrase (10 to 79 bytes) is read from the environment variable
  named by `passphrase_env` when the flowgraph starts. It is never stored
  in a flowgraph file, a status snapshot or a log line.
- An environment variable is a development convenience, not a secret
  manager.
- A wrong, missing or unexpected passphrase is a terminal `credentials`
  failure on the Caller, within about two seconds, with no data delivered.
- `security="none"` is an explicit local-test mode. Nothing ever falls back
  to it.

## How a run ends

| What happened | Sink | Source |
|---|---|---|
| Upstream finished | Flushes, sends END, waits up to `stop_timeout_ms` for SRT to take it. `session.completed` true | Emits all queued samples, then finishes. `session.end` is `complete` or `incomplete` |
| Flowgraph stopped by the user | No END. `completed` false, `failure` none | Strict: fails with `connection_lost`, `termination_unknown` true. Segmented: waits for a new connection within the reconnect budget |
| Terminal failure | Finishes at once. `session.failure` says why | Finishes at once. `session.failure` says why |
| Upstream finished while a segmented Sink had no connection | Waits up to `stop_timeout_ms`, then finishes with `completed` false, `failure` none, `state` `connecting`. Almost everything it consumed has expired; see `tx_queue.dropped_expired_samples` | Nothing arrives |
| The two ends disagree on sample rate or format | Fails with `configuration` about two latencies after the start; `failure_detail` is `reported by the peer: ` followed by the Source's reason. It does not retry | Fails at once with `configuration`; `failure_detail` names the difference. It tells the Sink, and keeps the connection open for one latency so that the message arrives |
| A strict Source saw a loss | Fails with `continuity`, `reported by the peer: ...`, in either mode | Fails with `continuity` |

When a run fails, read `session.failure_detail` on **both** ends.

- The Source tells the Sink when it ends a run because of what it received
  (`configuration`, `protocol`, `continuity`, or its own queue overflowing).
  This is one message and best effort: a run shorter than about two
  latencies can complete on the Sink before the message arrives, and a peer
  running 0.1.0-rc1 neither sends nor reads it. A Sink without such a
  message knows nothing about the receiver.
- A wrong passphrase is visible on the Caller only.

**A finished flowgraph is not a successful run.** GNU Radio has no failure
channel for a block, so both blocks finish (`WORK_DONE`) on failure as well
as on completion. Always read `status()` afterwards.

- `session.end == "complete"` on the Source means: observed from a clean
  start, no gap, END received. It is a statement about the receiver. The
  Sink never learns it and END is not acknowledged.
- When a Sink ends abruptly (failure or user stop) the connection closes at
  once. Samples it sent within the last latency window may not be delivered.
- END is the last packet, and SRT cannot notice the loss of a last packet.
  The Sink therefore sends it three times, 10 ms apart
  (`counters.end_repeats_sent`); the Source acts on the first that arrives.
  If all three are lost the Source reports `termination_unknown`, never
  completion.

## Outages and vanished peers

Tested with a fault-injecting relay on loopback, not on a real network.

| Situation | Segmented mode | Strict mode |
|---|---|---|
| Outage shorter than `peer_idle_timeout_ms` | The connection survives. Samples that became too late are missing; the Source tags the gap with its exact size | The Source fails with `continuity` at the first gap and tells the Sink, which fails with `continuity` too |
| Outage longer than `peer_idle_timeout_ms` | Both ends reconnect. Run and coordinates continue; the Source tags the boundary `reconnect` with the exact gap | Both ends fail |
| Peer vanishes without closing | Loss is noticed after the idle timeout. The block retries within `reconnect_total_timeout_ms`, then fails with `reconnect_timeout` | Fails after the idle timeout |
| All three END transmissions lost | The Source waits for the sender to return until `reconnect_total_timeout_ms` (60 s by default) has passed, then reports `termination_unknown` and fails with `reconnect_timeout`. With a single END (0.1.0-rc1) this happened in 1 of 6 runs of 120 s at 1 % loss | The Source fails with `connection_lost`, `termination_unknown` true |
| Final record lost, END arrives | `end` is `incomplete`; the missing tail is counted and published as a status event | The Source fails with `continuity` |

Things to know:

- **By default, age is not bounded when the network queues instead of
  dropping.** If a bottleneck in front of a deep queue carries less than
  the stream for seconds, nothing is lost, so nothing is dropped, and
  samples arrive seconds old (measured in emulation: up to 6.3 s).
  `latency_ms` bounds delay only while the path keeps up. Set
  `max_lateness_ms` on the Source to bound it; see "Bounding the age".
- During an outage a segmented Sink keeps consuming and discards records
  older than `queue_max_age_ms`. It does not build a backlog and does not
  replay old samples after the path returns.
- A strict Sink sees only its own side. It cannot observe samples lost in
  the network; the Source can. Run the Source in strict mode too if any
  loss must end the run.
- `session.end_acknowledged_by_transport` on the Sink is true when, after
  END, SRT reported nothing left unacknowledged. The peer's SRT stack then
  received every packet. It is not an acknowledgement by the receiving
  application, and false does not prove a loss.
- A Listener serves one peer. A second Caller, or one with a different
  `stream_id`, is refused during the SRT handshake. The refused Caller
  treats a wrong `stream_id` as a terminal `configuration` failure and a
  busy peer as temporary.

## Bounding the age

SRT schedules every message for delivery one latency after it was sent. A
message that arrives after that time is handed over at once, however late.
`max_lateness_ms` on the Source sets how late is still acceptable:

- A DATA record handed over later than that is discarded. The Source never
  sees its samples; the next record that is on time carries a
  `rwsrt.discontinuity` tag with the exact number of missing samples, as
  for any other loss.
- `counters.late_records_dropped` and `late_samples_dropped` count what was
  discarded. `counters.max_lateness_seen_ms` is the largest lateness seen,
  whether or not a bound is set; a value far above a few milliseconds means
  the path is queueing.
- Lateness is measured on SRT's clock of the receiving host, against SRT's
  own schedule. No clock of the sender is involved.
- With the bound, the age of a delivered sample at the Source's output is
  at most about `latency_ms` plus `max_lateness_ms` plus the path delay.
- The bound does not make more samples arrive on time. What was late is
  missing either way; the bound only keeps stale samples away from
  downstream processing.
- In strict mode a discarded record is a loss and ends the run.
- END is never discarded.
- The default is 0, off: nothing is discarded for being late. A value of
  about half the latency is a reasonable start. Too small a value discards
  records on an ordinary path; lateness on a path that keeps up was about
  2 ms in emulation.

## Status

```python
status = sink.status()          # dict
text = sink.status_json()       # the same as one JSON object
```

The `status` message port publishes the same content as a PMT dictionary
every `status_interval_ms` while the block runs. The Source also publishes
an `event` message for a break that has no sample after it. The snapshot
published while a flowgraph shuts down may not reach subscribers inside
that flowgraph; `status()` after the run is the reliable final state.

Top-level fields of both blocks: `block`, `block_id`, `direction`,
`running`, `run_id`, `segment_id`, `next_sample_index` (the coordinate of
the next sample), `capture_time_tags`, `last_stop_ms` (how long the last
stop took), `config`, `tx_queue` or `rx_queue`, `session`, `versions`
(`module`, `gnuradio`, `robotweax_srt`) and `srt_runtime_leases` (running
blocks of this module in the process).

Sink only:

| Field | Meaning |
|---|---|
| `input_samples_consumed` | Samples taken from the input port |
| `records_assembled`, `partial_record_pending` | Records built from them; whether samples wait for a record to fill |
| `upstream_known_gaps`, `upstream_known_gap_samples`, `upstream_unknown_gaps` | Gaps announced by input tags |
| `untransported_tags`, `malformed_tags` | Input tags that are not carried; `rwsrt.*` input tags that could not be read |
| `natural_end`, `end_enqueued` | Upstream finished; the END record was queued |

Source only:

| Field | Meaning |
|---|---|
| `output_samples_emitted`, `records_emitted` | Samples and records written to the output port |
| `discontinuity_tags` | `rwsrt.discontinuity` tags written. Can be lower than `session.continuity.discontinuities` while breaks are still queued |
| `untagged_discontinuities` | Breaks with no sample after them, published as `event` messages instead |
| `reported_missing_samples` | Sum of the known gap sizes in those tags and events |
| `unknown_breaks` | Breaks whose size is not known, including an `initial_join` |
| `receiver_queue_drop_breaks` | Breaks caused by this block's own full queue |
| `end_emitted` | The END record was reached |

`tx_queue` and `rx_queue` hold the current and highest fill and what was
dropped: `dropped_overflow_*` because the queue was full, `dropped_expired_*`
because a record was older than `queue_max_age_ms`.

`session` holds `state`, `failure`, `failure_detail`, `completed`, `end`,
`termination_unknown`, `connection_generation`, and four groups:

| Group | Content |
|---|---|
| `counters` | Connection attempts and losses, rejected peers, `records_send_accepted`, `samples_send_accepted`, records and bytes received, malformed records, `late_records_dropped`, `late_samples_dropped`, `max_lateness_seen_ms`, `end_sent`, `end_repeats_sent`, `peer_notices_sent`, `peer_notices_received` |
| `continuity` | Source: samples accepted, known missing samples, unknown gaps, discontinuities, runs |
| `effective` | Option values read back from SRT: latencies, payload size, buffers, bandwidth settings, key state |
| `link` | SRT statistics: RTT, packets, loss, retransmissions, drops |

`session.end` belongs to the Source; on a Sink it is always `none`, and a
successful Sink shows `completed` true. `error_code` is the last SRT error
number, 0 if none. Values in `effective` and `link` are -1 or zero until a
connection exists. `rejected_peers` counts peers the Listener refused for
their `stream_id` or because it was busy; **a Caller with a wrong
passphrase is rejected inside SRT and is not counted, so the Listener
shows no sign of it** and ends with `startup_timeout` if no valid peer
comes.

Counters are named by layer. `samples_send_accepted` means accepted into
SRT's local buffer, not received. SRT packet loss in `link` and missing
samples in `continuity` are different measurements.

Failure values: `startup_timeout`, `reconnect_timeout`, `connection_lost`,
`credentials`, `configuration`, `protocol`, `continuity`, `queue_overflow`,
`transport`, `internal`.

## Stop time

Stopping a block from outside (`tb.stop()`) takes at most a few wait slices
of 20 ms. Measured with no peer, both roles and both policies: at most
52 ms, against a bound of 1000 ms. This is not the time a Sink takes to
finish after its upstream ends: it then waits up to `stop_timeout_ms`
(default 1000) for END to be taken, and the whole of it when there is no
connection. This covers these two blocks only. A flowgraph's stop time also
depends on its other blocks; for example `blocks.throttle` at a very low
rate sleeps uninterruptibly.

## Synthetic sources must be paced

The Sink expects a stream at the declared sample rate. An unthrottled signal
generator offers data as fast as the CPU allows; in strict mode that
overflows the queue and fails the run, in segmented mode it discards most
samples. Put a throttle after synthetic sources. Real hardware paces itself.

## Not in this version

Vector ports, more than one stream per block, DNS names, IPv6, rendezvous,
SRT File mode, FEC or bonding settings, authenticated encryption, automatic
resampling or clock recovery, and interpretation of UHD-style `rx_time`
tags.
