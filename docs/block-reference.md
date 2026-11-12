# Block reference

Two blocks: **SRT Sink** (`robotweax_srt.srt_sink`) sends one sample stream,
**SRT Source** (`robotweax_srt.srt_source`) receives it. Both ends must run
this module; an ordinary SRT media player does not understand the payload
([protocol-v1.md](protocol-v1.md)). Tags and time are described in
[continuity-and-time.md](continuity-and-time.md).

Status of this document: it describes what is implemented and tested on
loopback. Nothing here has been measured under network impairment.

## Construction

```python
from gnuradio import robotweax_srt

transport = robotweax_srt.make_transport_config(
    role="caller", host="192.0.2.10", port=9000, passphrase_env="RWSRT_PASSPHRASE")
stream = robotweax_srt.make_stream_config(format="cf32", sample_rate_num=100000)
sink = robotweax_srt.srt_sink(transport, stream)
```

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
| `reconnect` | true | Used in segmented mode only. Strict mode never reconnects |
| `reconnect_initial_ms`, `reconnect_max_ms` | 250, 4000 | Exponential retry delay with 20 % jitter |
| `reconnect_total_timeout_ms` | 60000 | Budget after an established connection failed |
| `stop_timeout_ms` | 1000 | Bound for handing END to SRT at a natural end |
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

**A finished flowgraph is not a successful run.** GNU Radio has no failure
channel for a block, so both blocks finish (`WORK_DONE`) on failure as well
as on completion. Always read `status()` afterwards.

- `session.end == "complete"` on the Source means: observed from a clean
  start, no gap, END received. It is a statement about the receiver. The
  Sink never learns it and END is not acknowledged.
- When a Sink ends abruptly (failure or user stop) the connection closes at
  once. Samples it sent within the last latency window may not be delivered.
- END itself is an ordinary live packet and can be lost. The Source then
  reports `termination_unknown`, never completion.

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

Top-level fields: `block`, `direction`, `running`, `run_id`, `segment_id`,
`next_sample_index`, block counters, `config`, `tx_queue` or `rx_queue`,
`session`, `versions`, `srt_runtime_leases`.

`session` holds `state`, `failure`, `failure_detail`, `completed`, `end`,
`termination_unknown`, `connection_generation`, and four groups:

| Group | Content |
|---|---|
| `counters` | Connection attempts and losses, rejected peers, `records_send_accepted`, `samples_send_accepted`, records and bytes received, malformed records, `end_sent` |
| `continuity` | Source: samples accepted, known missing samples, unknown gaps, discontinuities, runs |
| `effective` | Option values read back from SRT: latencies, payload size, buffers, bandwidth settings, key state |
| `link` | SRT statistics: RTT, packets, loss, retransmissions, drops |

Counters are named by layer. `samples_send_accepted` means accepted into
SRT's local buffer, not received. SRT packet loss in `link` and missing
samples in `continuity` are different measurements.

Failure values: `startup_timeout`, `reconnect_timeout`, `connection_lost`,
`credentials`, `configuration`, `protocol`, `continuity`, `queue_overflow`,
`transport`, `internal`.

## Stop time

Stopping either block takes at most a few wait slices of 20 ms. Measured
with no peer, both roles and both policies: at most 52 ms, against a bound
of 1000 ms. This covers these two blocks only. A flowgraph's stop time also
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
