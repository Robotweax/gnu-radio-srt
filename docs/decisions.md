# Decisions and M0 findings

Records what milestone M0 established on the pinned platform, the decisions
taken, and where they amend [project-plan.md](project-plan.md) (v1.0). The
plan file is kept as received; this file wins where the two differ.

Numbers are measurements from one host inside a container. They show that a
mechanism works and roughly how it behaves. They are not performance claims.
Evidence: `tools/dev/run.sh tools/dev/gate.sh`, summaries in
[evidence/m0/](evidence/m0/), [evidence/m1/](evidence/m1/),
[evidence/m2/](evidence/m2/), [evidence/m3/](evidence/m3/),
[evidence/m4/](evidence/m4/) and [evidence/m5/](evidence/m5/).

## Status of the M0 blocking checks

| Check (plan section 2) | Result | Evidence |
|---|---|---|
| Public-header compile | pass | `spikes/m0_srt_api` builds with `-Wall -Wextra -Wpedantic` against `<srt/srt.h>` only |
| Package discovery | pass | `find_package(RobotweaxSRT 0.3 CONFIG REQUIRED)` resolves 0.3.2 |
| Python import | pass | import from a clean prefix, outside source and build trees |
| Companion block discovery | pass | GRC platform lists both blocks; `grcc` output runs and exits 0 |
| Nonblocking connect/accept/send/receive | pass | 64 records of 1200 bytes, plaintext and CTR, intact |
| Epoll cancellation strategy | pass | D6 |
| Socket-option readback | pass | D9, D10 |
| Runtime stop with no peer | pass | D6 |
| EOF mechanism (plan section 6) | pass | D5 |
| Shared SRT runtime (plan section 8) | pass | D8 |

M0 exit gate: met on Linux aarch64 (local) and on x86-64 (hosted CI runner,
D2).

## D1. Robotweax SRT pin

- Pinned: tag `v0.3.2`, commit `e45d0edd346e5c427d85e4c3b1ac605c81a23397`.
- The plan's inspected commit `09ceab33` is the 0.2.8 release merge of
  2026-10-08. Three releases followed; 0.3 is a new ABI line. Starting a new
  module on the older line would need a migration before v0.1.
- Consequence: `find_package(RobotweaxSRT 0.3 CONFIG REQUIRED)`, not `0.2`.
- Build: Release, shared library, default namespaced layout
  (`librobotweax-srt.so.0.3`), OpenSSL 3.0.13 backend, AES-GCM preview off.
- Identity as measured: package and header report 0.3.2;
  `srt_getversion()` reports 1.5.7 (the compatible API value). Product
  identity is taken from the package, never from `srt_getversion()`.
- The image builds SRT from `git archive <commit>`, so the state of the
  local SRT working tree does not matter. The gate fails if the installed
  commit differs from the lockfile.
- `main` of the SRT repository has moved on (hybrid multicast, unreleased).
  It is deliberately not followed.

## D2. Qualified platform (deviation from the plan)

- Plan: Ubuntu 24.04 x86-64 first, arm64 second.
- Done: Ubuntu 24.04 **aarch64** in Docker on an Apple Silicon host
  (kernel 7.0.14-linuxkit, 16 CPUs). This is the hardware available.
- x86-64, the plan's primary platform, is qualified on a GitHub-hosted
  `ubuntu-24.04` runner by owner decision of 2026-11-12
  (`.github/workflows/gate.yml`, every push). First run: all M0 gates passed
  (run 43218140005; Intel Xeon Platinum 8573C, 2 vCPUs, kernel
  7.0.0-1017-azure). Its figures agree with aarch64: stop with no peer at most
  11 ms over 100 cycles, source delivery 209 ms when returning zero items
  against 0.07 ms when waiting inside `work()`.
- A hosted runner is shared and virtualised. It shows that the gates hold
  there; it is not a reference host for timing claims. That needs dedicated
  hardware (M4).
- All timing figures in this file come from containers and say nothing about
  a bare-metal host.

## D3. GNU Radio and toolchain

GNU Radio 3.10.9.2 from the Ubuntu 24.04 archive (`3.10.9.2-1.1ubuntu2`),
GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.12.3, pybind11 2.11.1,
NumPy 1.26.4. Ubuntu packages are fixed by the base image digest plus the
archive state at image build time; each gate run records the installed
versions in its `manifest.json`.

## D4. Names

| Thing | Name |
|---|---|
| Repository | `gr-robotweax-srt` |
| GNU Radio module / CMake project | `robotweax_srt` / `gr-robotweax_srt` |
| Library and CMake target | `gnuradio-robotweax_srt`, `gnuradio::gnuradio-robotweax_srt` |
| C++ namespace | `gr::robotweax_srt` |
| Python package | `gnuradio.robotweax_srt` |
| Companion category | `[Robotweax SRT]` |

The plan's fragment used the target name `gnuradio-robotweax-srt`. The
`gr_modtool` template derives every name from the module name, which cannot
contain a hyphen, so the template names are kept. The module compiles as
C++20 privately; installed headers stay usable from C++17 consumers and
include no SRT header.

`python/robotweax_srt/__init__.py` no longer swallows a failed binding
import, as the template does: a missing binding is an installation error.

## D5. Natural EOF versus cancellation (plan sections 6 and 9)

Mechanism on GNU Radio 3.10.9.2: inside the block's `stop()`,

```cpp
detail()->input(0)->done() && detail()->input(0)->items_available() == 0
```

is true exactly when upstream finished and every input item was consumed.

| Case | `done` at stop | pending | work calls |
|---|---|---|---|
| `vector_source` of 100000 items | true | 0 | 14 |
| `null_source` then `head(4113)` | true | 0 | 1 or 2 |
| `head(0)`, no sample ever | true | 0 | 0 |
| endless source, `tb.stop()` | false | 0 | 14 |
| no samples, `tb.stop()` | false | 0 | 0 |

Consequences for the Sink design:

1. At natural EOF the scheduler does **not** call `general_work()` again;
   with an empty finite run it never calls it at all. Flushing the partial
   record and enqueueing END therefore happens in `stop()`, which then waits
   (bounded by `stop_timeout_ms`) for the worker to hand them to SRT.
2. `stop()` runs once per start, on the scheduler thread, for both natural
   completion and cancellation. The predicate above is the only difference.
3. A user stop that arrives after upstream finished and all input was
   consumed is reported as natural completion. That is accurate: nothing was
   cut off.
4. This uses `block_detail` and `buffer_reader`, which are installed public
   headers but scheduler internals. The QA tests pin the behaviour; a
   GNU Radio update that changes it fails them.

Not covered by M0: a multi-input block, and message-port-driven completion.
Neither is in v0.1 scope.

## D6. Cancellation and bounded stop (plan section 9)

- Worker waits use `srt_epoll_uwait(..., 20 ms)` and re-check an atomic
  cancel flag. Sockets and the epoll instance are closed by the worker that
  owns them, then the worker is joined, then the SRT runtime lease is
  released.
- Closing a socket with a pending nonblocking connect and releasing its
  epoll, and closing an idle listener: under 1 ms each.
- `srt_epoll_release()` from another thread wakes a waiter blocked for 5 s
  in under 1 ms (the waiter gets `SRT_EINVPOLLID`). This is available as a
  fallback wake-up; the design does not depend on it.
- Block `stop()` with no peer, Caller and Listener: worst observed 31 ms over
  more than 1500 start/stop cycles across all runs; bound is 1000 ms.
- 100 start/stop cycles per run: thread count and open descriptors return to
  baseline. One early run showed one extra thread immediately after the last
  cycle; no later run did. The test now samples immediately and after a
  settle period and records both. **Watch item**, not explained.

**Amendment to the plan.** A 20 ms `srt_epoll_uwait` was observed to return
after up to 31 ms. "Socket waits are bounded to at most 20 ms" becomes: waits
request 20 ms and are budgeted at 35 ms.

**Finding outside this module.** `blocks.throttle` at a very low rate sleeps
uninterruptibly and held `tb.stop()` for minutes in an early test. The stop
bound in this project covers this module's blocks only; a flowgraph's stop
time also depends on its other blocks. To be stated in the block reference.

## D7. Source with an empty queue (amends plan section 9)

The plan lets the Source wait at most 20 ms per scheduler call and return.
Measured on the pinned scheduler:

| Strategy | Injected data reaches downstream after | Idle calls per second |
|---|---|---|
| Wait 20 ms, return 0 items | 207 to 227 ms (worst case per run) | about 4 |
| Keep waiting inside `work()` in 20 ms slices | at most 1.4 ms | 1 call, parked |

A source that returns 0 items is not called again for roughly 260 ms. That
would add up to a quarter second of latency and jitter, which is the size of
the default SRT latency budget.

**Decision.** The Source waits inside `work()` in slices of at most 20 ms
until data, END, a terminal error or cancellation. Cancellation is detected
by polling `boost::this_thread::interruption_requested()` between slices
(`top_block::stop()` interrupts the scheduler thread; a
`std::condition_variable` wait is not a boost interruption point). On
interruption `work()` returns 0 and the scheduler loop exits. Measured: stop
within 20 ms while waiting, CPU cost negligible, `WORK_DONE` ends `tb.run()`
with all queued items delivered first.

The return-zero behaviour is pinned by a characterisation test so that a
runtime change is noticed.

## D8. SRT runtime ownership (plan section 8)

- `srt_startup()` / `srt_cleanup()` are reference counted in 0.3.2: two
  startups, one cleanup, and the runtime is still usable; after the final
  cleanup a new startup works. Final cleanup took about 1 ms or less.
- `lib/runtime_owner` holds one process reference while any worker of this
  module holds a lease, and releases it after the last worker has closed its
  handles. Two blocks in one process connected to each other; stopping one
  left the other's connection and setup untouched (leases 2, then 1, then 0).
- Not tested, so not claimed: sharing a process with another component that
  embeds SRT itself, and loading a second SRT implementation. The installed
  module links exactly one SRT library; the gate checks that.

## D9. Payload ceiling and record size (plan sections 5 and 8)

IPv4, MSS 1500, Live/Message mode:

| Mode | Largest accepted `SRTO_PAYLOADSIZE` | Largest single message |
|---|---|---|
| Plaintext | 1456 | 1456 |
| AES-CTR, 16-byte key | 1456 | 1456 |

The default 1200-byte record is accepted in both modes. The effective
payload size read back after connection equals the configured value. CTR adds
no per-packet payload overhead here. Any other MSS, address family, key
length, or packet filter needs its own measurement before it is allowed.

Receiving into a buffer smaller than a message fails with
`SRT_EINVALMSGAPI`, so the receive buffer is sized to the configured ceiling.

## D10. Effective socket values and open settings

Read back on a configured, unconnected socket: latency 250/250 ms, MSS 1500,
TSBPD on, too-late drop on, linger off, enforced encryption on, connect
timeout 3000 ms, peer idle timeout 5000 ms, stream ID as set. After the
handshake both ends report receiver latency 250 ms and the Listener sees the
Caller's stream ID.

Library defaults that the plan wants bounded explicitly, still to decide in
M1 with the queue design:

| Option | Default read back | Note |
|---|---|---|
| `SRTO_SNDBUF`, `SRTO_RCVBUF` | 12058624 bytes each | Larger than the 4 MiB application queue |
| `SRTO_FC` | 25600 packets | |
| `SRTO_UDP_SNDBUF` | 65536 bytes | |
| `SRTO_UDP_RCVBUF` | 12288000 requested, 4194304 effective | Capped by `net.core.rmem_max` in this container |

The library logs a warning to the process output for every socket whose UDP
receive buffer is capped. **To do in M1:** install an SRT log handler so
library output goes to the module's diagnostics, and either request a UDP
buffer within the kernel limit or document the sysctl.

## D11. Failure classification seen at the API (input to plan section 9)

| Situation | What the API shows | Class |
|---|---|---|
| Caller, nobody listening | `ERR` event after 3005 ms (`SRTO_CONNTIMEO`), state `BROKEN`, reject `SRT_REJ_TIMEOUT` | transient, retry |
| Wrong passphrase | no connection, about 11 ms, reject `SRT_REJ_BADSECRET` | terminal, credentials |
| Passphrase on one side only | no connection, about 11 ms, reject `SRT_REJ_UNSECURE` | terminal, credentials |
| Peer closes | `IN`+`ERR` event within 1 ms, receive fails with `SRT_ECONNLOST` | connection loss |
| Empty nonblocking receive / accept | immediate `SRT_EASYNCRCV` | retry |
| Wait on a released epoll | `SRT_EINVPOLLID` | cancellation |

With enforced encryption on both sides no data was delivered in any of the
three mismatch cases. Peer close was a clean close on loopback; a peer that
vanishes without closing is detected by the idle timeout and is tested in M3.

With `srctime = 0` and 250 ms latency, the first message was delivered to
the receiver 251 to 253 ms after submission on loopback, as TSBPD intends.

## D12. Licensing and packaging

The module is GPL-3.0-or-later. **Confirmed by the owner on 2026-11-12.**
Robotweax SRT's MIT notice is in `THIRD_PARTY_NOTICES.md`. Obligations of
binary packages are reviewed when the first binary package is produced.

Source build and clean-prefix install work with the template's CMake install
rules. The `.conda` recipe from the template was removed as unqualified.

## Open items after M0 (status updated in M1)

1. A dedicated x86-64 host for timing-sensitive qualification in M4 (D2).
   Functional x86-64 qualification runs in CI.
2. First user: none. The owner stated on 2026-11-12 that this is an academic
   project for now. The plan's "validate the user story with a
   measurement/SDR user" and "one independent developer reproduces the
   examples" gates therefore stay open and are not blocking; the synthetic
   deterministic signal is the workload until a user exists.
3. UDP receive buffer: still the library default request; see D10 update.
4. Done in M1: SRT log handler (D10 update).
5. Binary-package obligations, when a binary package is first built (D12).
6. Receiver idle/disconnect policy against the outage profile N5: the 5 s
   outage equals the default `SRTO_PEERIDLETIMEO` of 5000 ms, so the
   connection may or may not survive it. To be set deliberately.
7. Explained in M3: the thread-count watch item (D6 update).
8. Done in M1: sanitizer and fuzz builds (D21).
9. CI: `.github/workflows/gate.yml` runs the gate on a hosted
   `ubuntu-24.04` x86-64 runner on every push (owner decision 2026-11-12).
   The repository is `https://github.com/Robotweax/gnu-radio-srt` (private).

# Milestone M1: protocol and core

M1 exit gate (plan section 13): unit and fuzz suite, headless end-to-end
transfer, bounded allocations.

| Item | Result | Evidence |
|---|---|---|
| Wire codec with independent golden vectors | done | 5 fixtures from `tools/gen_golden_vectors.py`, byte-exact both ways |
| Unit suite | 60 cases pass | plain, ASan+UBSan, TSan |
| Mutation check of the unit suite | 16 of 16 seeded bugs detected | one-off, not part of the gate |
| Decoder fuzzing | no finding | about 54 million executions per 60 s run |
| Record-stream and state-machine fuzzing | no finding | about 9 million executions per 60 s run |
| Headless end-to-end transfer | 22 cases pass | plain, ASan+UBSan, TSan with instrumented SRT |
| Bounded queues | pass | no peer at 1 MS/s: queue never above its 512-record limit |
| x86-64 | all 9 gates pass | hosted runner, run 43225318124 on commit `2acf640`, 18 minutes |

Not part of M1 and not done: network impairment (M4), UDP and TCP baseline
peers (M4), memory growth over a long run (M3 soak), the GNU Radio blocks
themselves (M2). All end-to-end results are loopback without loss.

## D13. Protocol points fixed in `protocol-v1.md`

The plan left these open; version 1 now fixes them:

- A non-normalized rate is rejected, not normalized by the receiver.
- A capture time before the Unix epoch is impossible. So is an uncertainty,
  or a time plus uncertainty, beyond the signed 64-bit range.
- The capture-time flag and the clock domain must agree in both directions.
- `record_sequence` 2^64 - 1 is never used, so the sequence cannot wrap.
- The message bound is checked before any field is read.
- Encoder and decoder share one rule set (`validate()`); the fuzzer checks
  that anything the decoder accepts re-encodes to the same header bytes.

## D14. Code layout

- `lib/` holds the plan's files plus `sample_time`, `continuity`,
  `record_assembler`, `session_state` and `session_config`.
- Two static libraries defined in `cmake/RwsrtNative.cmake`: `rwsrt_core`
  (no GNU Radio, no SRT) and `rwsrt_transport` (the only code that includes
  `<srt/srt.h>`). Namespace `rwsrt`. The GNU Radio module links both.
- `tests/` is also a standalone CMake project, so core, transport and peers
  build without GNU Radio for sanitizers and fuzzing.
- Unit tests use header-only Boost.Test, which the GNU Radio template
  already depends on.

## D15. State machine as implemented

- Before the first connection, failed attempts are retried in both
  continuity modes until `startup_timeout_ms`. The plan's diagram shows
  retry only after a connection; without startup retries a Caller started
  before its Listener would fail on the first attempt.
- After a connection, a loss is retried only in segmented mode with
  reconnect enabled, within `reconnect_total_timeout_ms`.
- STARTING is not observable: resources exist before the first attempt.
- COMPLETED and FAILED are kept until `stop()`. The first failure is
  retained; later errors, including errors during teardown, never replace it.
- A credential or configuration rejection is terminal and not retried
  (measured: failure reported within 2 s, no second attempt).

## D16. Strict mode before the first connection (input to M2)

A strict sender must not accept samples before a peer is connected: its
queue would overflow or age out within `queue_max_age_ms` and the run would
fail at once. The reference sender therefore waits for the connection before
producing. **For the M2 Sink:** in strict mode `general_work()` consumes
nothing until the worker is connected, which pushes back on upstream. In
segmented mode samples are accepted from the start, old records are
discarded, and a receiver that connects later reports `initial_join`.

## D17. Queue behaviour

- Limits apply to DATA: bytes, records, age. A record exactly at the age
  limit is still live.
- END has reserved capacity: always admitted, never evicted, never expired,
  never overtakes DATA. Nothing is accepted after END.
- Strict: a full queue or an expired record is a terminal error and nothing
  is displaced.
- Segmented: oldest DATA is discarded. On the receive side the next record
  that leaves the queue carries `receiver_queue_drop` with the exact number
  of discarded samples, merged with any break the discarded records carried
  themselves (unknown wins over known). Measured in the stall test: 100050
  samples discarded, 100050 reported, received plus reported equals sent.

## D18. END delivery and connection loss on the sender

- Linger is off, so closing discards what SRT still holds. After END is
  accepted by SRT the worker waits until `SRTO_SNDDATA` reaches zero, at
  most latency plus `stop_timeout_ms`, before closing. This gives END its
  chance; it is still not an acknowledgement.
- END that was taken from the queue but not yet accepted by SRT is kept
  across a reconnect.
- A DATA record in the same position is counted as a stale drop and not
  resent: after an outage it is no longer current.
- A sender whose receiver went away keeps trying until the reconnect budget
  ends, then fails with `reconnect_timeout`. It never reports completion.

## D19. Listener admission

- One active peer. Further peers are accepted and closed at once, counted
  as `rejected_peers`.
- A peer with a different stream ID is closed right after accept. The
  rejection therefore happens after the SRT handshake, not inside it: the
  caller briefly sees a connection. **To do in M3:** reject in the handshake
  with a listen callback.

## D20. Bandwidth setting

The sender sets `SRTO_MAXBW = 0` and `SRTO_INPUTBW` to an upper bound of
its byte rate, framing included: payload, plus one header per full record
and per flush interval. Example: cf32 at 200 kS/s gives 1787296 bytes/s
(1.6 MB/s payload). The overhead reserve is the library default,
`SRTO_OHEADBW` 25 %, and is read back into the status snapshot. Whether 25 %
is enough for retransmission under loss is an M4 question; nothing here
measures it.

## D10 update. Buffers and library log

- `SRTO_SNDBUF` and `SRTO_RCVBUF` are set explicitly from
  `srt_buffer_bytes` (default 8 MiB; read back as 8387456 after rounding to
  packet slots). Flow window stays at the default 25600 packets.
- While the module holds the SRT runtime, library log output goes to a
  handler in `runtime_owner` (message count and last message, in the status
  output) instead of stderr. `RWSRT_SRT_LOG=1` prints it.
- Still open: the UDP receive buffer. The library requests 12288000 bytes
  and the option reads back that request, not the 4194304 the kernel
  granted in this container. The effective value is only visible in the
  library's log line.

## D21. Sanitizers and fuzzing

- Unit tests: ASan+UBSan and TSan, GCC.
- End-to-end suite: ASan+UBSan against the normal SRT build.
- End-to-end suite under TSan needs a TSan-built SRT. Against the normal
  library TSan reported data races inside SRT's listener and channel
  teardown; against a TSan-instrumented build of the same commit the whole
  suite ran with zero reports. Those reports were artefacts of the
  uninstrumented library. `tools/dev/tsan-e2e.sh` builds the instrumented
  library and is part of the gate.
- Fuzz targets (clang, libFuzzer, ASan+UBSan): `fuzz_decode` and
  `fuzz_record_stream`. The gate runs each for 60 s. Longer runs and a kept
  corpus are not set up.
- Found on the way, both in test code: a Boost.Test assertion used from a
  second thread (a real race in the test), and a fuzz invariant that was
  stricter than the specification.

## D22. Observations, not results

- Under TSan the sender could not sustain 20000 one-sample records per
  second; strict mode reported `queue_overflow` rather than dropping. The
  case now runs at 2000 records per second because it tests framing, not
  rate. Records per second, not samples per second, is the cost driver of
  the worker loop. To be measured properly in M4.
- Worker wait slices in the end-to-end suite: at most 28 ms. Stop: at most
  26 ms. Both inside the budgets of D6.

## Open items carried into M2

1. Dedicated x86-64 host for M4 timing work (D2).
2. No first user: academic project for now (owner, 2026-11-12).
3. UDP receive buffer request and its effective value (D10 update).
4. Stream ID rejection inside the handshake (D19), M3.
5. Receiver idle timeout against outage profile N5: still the 5000 ms
   default, equal to the outage length.
6. The thread-count watch item of D6: not seen again in any run.
7. Longer fuzz runs with a persistent corpus.
8. `rx_time`-style adapters and unknown-tag accounting belong to M2 with
   the Sink's tag handling.

# Milestone M2: GNU Radio blocks

M2 exit gate (plan section 13): both data types and topologies; correct
partial I/O, EOF and discontinuities.

| Item | Result | Evidence |
|---|---|---|
| f32 and cf32, Sink Caller and Sink Listener | pass | `qa_transfer`, exact values, counts and record tags |
| Float bit patterns including NaN payloads | pass | `qa_transfer` |
| Partial output (37 items per scheduler call) | pass | each tag once, at its exact offset |
| Natural EOF, empty run, flush interval | pass | `qa_transfer` |
| Input tags: capture time, known gap, unknown gap | pass | `qa_tags` |
| Discontinuity tags: `sample_gap`, `upstream_unknown_gap`, `initial_join`, `new_run` with `reconnect`, `receiver_queue_drop` | pass | `qa_tags`, `qa_lifecycle` |
| Strict mode fails on the first violation, both blocks | pass | `qa_tags`, `qa_lifecycle` |
| Cancellation is not completion | pass | `qa_lifecycle` |
| Stop with no peer, 8 block/role/policy combinations | at most 52 ms over 4 runs | `qa_lifecycle` |
| 30 start/stop cycles of a connected pair | no thread or descriptor left | `qa_lifecycle` |
| Bindings, Companion blocks, generated flowgraphs, headless examples, C++ consumer | pass | `tools/dev/check-install.sh` |

39 flowgraph tests in five suites, all on loopback without loss. The full gate
also passes on the hosted x86-64 runner (run 43236052294 on commit
`e389066`, 18 minutes).

Not covered in M2:

- `reconnect` as the only reason (same run across an outage). It needs a
  network outage, which is M3 and M4. The ledger logic is unit-tested.
- Sanitizer runs of the block code inside GNU Radio. The native code under
  the blocks is covered (D21); the block adapters themselves are not.
- The gap-aware spectrum demonstration (`DEMO-01`). The headless receiver
  example has a minimal gap-aware consumer instead.
- Adapters for UHD-style `rx_time` tags.

## D23. Public API as built

- `srt_sink::make(transport_config, stream_config)` and the same for
  `srt_source`, as in the plan. The two configuration structs are public in
  `config.h` and bound to Python.
- Status: `status_json()` and `status_pmt()` in C++; `status()` returns a
  dictionary in Python. One description (`rwsrt::status_doc`) produces both
  forms, so they cannot drift apart. This replaces the plan's
  `status_snapshot status() const`.
- `make_transport_config(**kw)` and `make_stream_config(**kw)` reject
  unknown names and enumeration values. Companion uses them, so Companion
  and scripts share one validation path.

## D24. `start()` must not throw

GNU Radio 3.10 calls a block's `start()` on that block's scheduler thread.
An exception there kills the thread and the flowgraph hangs; it does not
reach the caller of `top_block.start()`. Found with a missing passphrase
variable.

- Every configuration value is checked at construction, where an exception
  does reach the caller.
- The passphrase is read at start, as the plan requires. If the variable is
  missing or its content invalid, the block records a terminal
  `configuration` failure naming the variable (never its content), logs it,
  and finishes at once. Measured: the flowgraph ends within 0.1 s.

## D25. How failure reaches the user

A block has no failure channel to the scheduler. Both blocks return
`WORK_DONE` on terminal failure. Measured: a Sink returning `WORK_DONE` ends
the whole flowgraph, including upstream blocks. So a failed run and a
completed run both end the flowgraph, and only `status()` tells them apart
(`session.completed`, `session.failure`, `session.end`). No END is sent and
no completion is reported on failure. This is stated prominently in the
block reference and in the Companion documentation of both blocks.

A failure reported from the scheduler thread is visible in the next
`status()` call, without waiting for the worker thread to act on it.

## D26. An abrupt end loses the last latency window

When a Sink fails or is stopped, its connection closes at once and SRT
discards what it holds (linger is off). At the receiver, packets still
waiting for their playout time are lost with the connection. Measured in
the strict upstream-gap test: 3000 samples consumed and accepted by SRT,
0 delivered. In another run a user stop delivered everything. Both are
valid outcomes.

Decision: no drain on failure or cancellation. A prompt, bounded stop wins,
and the plan makes no delivery promise for a cancelled run. Only a natural
end drains (D18). Documented in the block reference.

## D27. Status messages at shutdown

`stop()` publishes a final snapshot, and the Source publishes one before it
returns `WORK_DONE`. Subscribers inside the same flowgraph may already be
shutting down and may not receive either. The message port is for
monitoring a running block; `status()` after the run is the final state.

## D28. Tag contract as built

Formats and rules are in `continuity-and-time.md`. Points decided here:

- Tag values are PMT dictionaries with unsigned integer fields.
- An unreadable capture time sets nothing. An unreadable discontinuity is
  treated as an unknown gap: ignoring it would hide a real break.
- In strict mode an upstream discontinuity tag fails the Sink with
  `continuity`, like a network gap fails the Source.
- An `rx_rate` tag that differs from the configured rate fails the Sink
  with `configuration`. An equal one is counted as untransported.
- An unknown missing count is expressed by the absence of
  `missing_samples`, never by zero.

## D29. Threads and locks in the blocks

- Sink: scheduler thread, session worker, and a housekeeping thread that
  seals an expired partial record (so the flush interval holds when
  upstream pauses) and publishes status. The plan gave the flush to the
  network worker; a separate thread keeps the worker free of block state.
  Neither extra thread touches stream items or tags.
- Source: scheduler thread, session worker, status thread.
- Lock order in the Sink is assembly, then transmit queue. The assembly
  lock and the block's state lock are never held together. No lock is held
  during a socket call or a message publication.
- The Source waits inside `work()` in 20 ms slices (D7) and takes only
  already-queued records once it has produced something in a call.

## D30. Probe blocks removed

`m0_probe_sink` and `m0_probe_source` are gone. What they pinned is kept:

- The EOF predicate of D5 is now exercised by the real Sink in every
  finite-run test, and cancellation by the user-stop tests.
- The scheduler behaviour of D7 is pinned by
  `qa_scheduler_assumptions.py` with a small Python source (4.0 calls per
  second measured).

## Open items carried into M3

1. Outage and reconnect within one run, peer crash, repeated reconnect.
2. Stream ID rejection inside the handshake (D19).
3. Receiver idle timeout against a 5 s outage (still the 5000 ms default).
4. UDP receive buffer request (D10 update).
5. Sanitizer runs of the block adapters inside GNU Radio.
6. 30-minute soak and memory growth.
7. Gap-aware spectrum demonstration.
8. No first user: academic project for now (owner, 2026-11-12).

# Milestone M3: robustness

M3 exit gate (plan section 13): mandatory integration gates and a 30-minute
soak. The integration gates are the list in plan section 12 B.

| Gate (plan 12 B) | Result | Where |
|---|---|---|
| f32 and cf32, both Caller/Listener assignments | pass | `qa_transfer` |
| Plaintext and CTR; wrong or missing key delivers nothing | pass | `qa_lifecycle`, native suite |
| Several block instances, independent start and stop | pass | `qa_robustness` (two pairs in one process) |
| Missing peer | pass | `qa_lifecycle` |
| Refused connection | pass | `qa_robustness`: second Caller and wrong stream ID refused in the handshake |
| Peer crash | pass | native suite: receiver killed with SIGKILL, then replaced; `qa_robustness`: vanished peer |
| Receiver stall | pass | `qa_lifecycle` slow consumer, both modes |
| Sender stall | pass | `qa_robustness`: upstream pauses 2 s inside a record |
| Repeated reconnect | pass | `qa_robustness`: three outages, four connections, one run |
| Natural EOF with a partial final record | pass | `qa_transfer` |
| Lost final DATA, lost END, close without END | pass | `qa_robustness`, `qa_lifecycle` |
| Clean-prefix C++ consumer, Python import, Companion | pass | `tools/dev/check-install.sh` |
| Exact equality without loss | pass | `qa_transfer` |
| Known gaps correctly bounded under impairment | partly | exact for scripted outages and drops; delay, jitter and rate limits are M4 |
| Normal stop paths within 1 s | pass | worst 52 ms; 855 ms when END is never acknowledged |
| 100 start/stop cycles | pass | `qa_lifecycle`, no thread or descriptor left |
| 30-minute run | pass (aarch64, one run) | `tools/dev/soak.sh`, D39 |

The block suites now hold 56 flowgraph tests in seven files; the native
end-to-end suite holds 26 cases. All of it is loopback. The full gate (11
checks) also passes on the hosted x86-64 runner: run 43254414141 on commit
`1b651ea`, 26 minutes.

Not covered in M3:

- Delay, jitter, reordering, bandwidth limits and realistic loss patterns
  (M4, netem).
- The strict Sink's reaction to SRT sender-side drops (D33): implemented,
  never triggered by any test.
- ThreadSanitizer on the block code inside GNU Radio (D37).
- The `--gui` mode of the spectrum example.

## D31. Fault injection

`tools/impair/udp_proxy.py` relays UDP between a Caller and a Listener and
applies scripted faults: outages, a blackhole from a given time, seeded
random loss, and dropping the packets that carry a given RWSR record (END,
or the record containing a given sample) together with their
retransmissions. It is deterministic and needs no privileges, so the same
tests run locally and on the hosted runner.

It is a fault injector, not a network emulator: no delay, jitter,
reordering or rate limit. Results obtained with it say that a mechanism
works, not how the module performs on a real path. Qualification uses
netem in M4.

## D32. Peer idle timeout and outages

`peer_idle_timeout_ms` is now a public parameter (default 5000, minimum
1000). It decides which of two behaviours an outage produces:

- **Shorter than the timeout:** the connection survives. With a 0.8 s
  outage at 200 kS/s the Source reported one gap of 101328 samples and the
  totals matched exactly (received plus reported missing equals sent).
- **Longer than the timeout:** both ends notice the loss and reconnect. Run
  and coordinates continue; the boundary is tagged `reconnect` with the
  exact gap. Three outages in a row gave connection generations 2, 3 and 4
  within one run.

Plan profile N5 in miniature (5 s outage, default timers, loopback): output
resumed 0.3 s after the path returned, far inside the 10 s budget. One
configuration on loopback; the N5 gate itself is M4.

During an outage a segmented Sink keeps consuming and discards aged
records: in the 2.5 s outage test it discarded about 299000 samples locally
and its queue stayed inside its limits. Nothing old is replayed afterwards.

## D33. What a strict Sink can and cannot see

In the short-outage test SRT's sender-side drop counter stayed at zero
while about 100000 samples were lost: the loss was decided at the receiver.
A strict Sink therefore cannot detect network loss on its own. In the
strict outage test the Source failed with `continuity` at the first gap and
the Sink failed with `connection_lost` only because the Source closed.

- For a run that must end on any loss, the **Source** must be strict.
  Stated in the block reference.
- The Sink additionally fails with `continuity` if SRT ever reports
  sender-side drops. That path is implemented and has not been triggered by
  any test.

## D34. END delivery, refined

- The wait for END to leave SRT is now bounded inside `stop_timeout_ms`
  (150 ms kept for teardown), so a natural end stops within the same bound
  as any other stop. Measured with END dropped on the path: 855 ms.
- New status field `end_acknowledged_by_transport`: after END, SRT reported
  nothing left unacknowledged. True on clean runs, false when END was
  dropped. It is transport-level only, and false does not prove a loss.
- With END lost the Source never reports completion: `termination_unknown`
  is true and the session fails (`connection_lost` in strict mode,
  `reconnect_timeout` in segmented mode). All samples had arrived in that
  test; only the end was unknown.
- With the final record lost but END delivered, END exposes the missing
  tail exactly (102 samples in the test). No sample follows, so there is no
  tag: the Source counts it in `untagged_discontinuities` and publishes a
  status event. Strict mode fails with `continuity`.

## D35. Admission in the handshake

Closes the weakness of D19. The Listener installs an SRT listen callback:

- wrong stream ID: rejected with reason 1404. The Caller classifies it as
  a terminal `configuration` failure. Measured: the Sink ended 0.04 s after
  start, with no connection and no sample consumed.
- already serving a peer: rejected with reason 1409. The Caller treats it
  as temporary and retries inside its startup timeout. The first peer's
  transfer was unaffected and exact.

Found on the way: when no further retry delay fitted into the startup
timeout, the Caller made back-to-back attempts until the deadline (15
attempts in 1.2 s). It now waits out the remainder (3 attempts).

## D36. UDP receive buffer

Closes the open point of D10. The module reads `net.core.rmem_max` and
requests no more than that for `SRTO_UDP_RCVBUF` (at most the library's
own default of 12288000 bytes), so the library no longer logs a capped
request. The kernel limit is in the status as
`effective.udp_rcv_buf_limit_bytes`. Whether a given limit is enough for a
given rate is an M4 question.

## D37. Sanitizers on the block code

`tools/dev/block-asan.sh` builds the GNU Radio module with ASan and UBSan,
preloads the sanitizer runtime into Python and runs the flowgraph suites.
Result: all pass, no report. It is part of the gate. Leak detection is off
there because the interpreter never frees everything; leaks are covered by
the native ASan runs and by the soak.

ThreadSanitizer inside GNU Radio was not attempted: neither Python nor the
GNU Radio runtime is instrumented, and D21 showed what that does to TSan's
reports. The native code under the blocks does run under TSan with an
instrumented SRT.

## D38. A GNU Radio pitfall met in testing

A test crashed with a segmentation fault inside GNU Radio's Python
gateway. Cause: a Python-implemented block was passed to `connect()`
without keeping a reference, was garbage-collected, and GNU Radio later
called into the freed object. This is GNU Radio behaviour for Python
blocks, not a defect of this module, and does not affect the C++ blocks.
The examples keep references to their Python blocks.

## D6 update. The thread-count watch item is explained

The occasional extra thread first seen in M0 appeared again in M3, in a
test that compared the thread count immediately after a flowgraph ended
(12 instead of 11, once in a full gate run). Repeating that suite 40 times
showed the pattern: in 16 of 160 checks one thread was still unwinding at
the first reading, and every time it was gone within 10 ms. It is a thread
finishing its exit, not a leak. All thread checks now use the rule the leak
tests already had: the count must return to its baseline within one
second, and any settling time is recorded. The 30-minute soak shows the
same flat count (D39).

## D39. Soak

`tools/dev/soak.sh` runs two sender/receiver pairs for 30 minutes with the
headless examples at 100 kS/s cf32 with AES-CTR, and samples memory,
threads and open descriptors of all four processes every 10 s. The
criteria are written in `tools/dev/soak.py` and were fixed before the run.

Result of the run on aarch64 (2026-11-12, one run):

| Run | Result |
|---|---|
| Clean, strict | 180000000 samples sent, 180000000 received, one contiguous stretch, receiver-observed complete, no SRT loss or retransmission |
| Outages, segmented, 7 s outage every 2 minutes | 15 outages, 16 connections on each side, one run. 169559594 received plus 10440406 reported missing equals 180000000. No unknown break. Queue high-water 90 records |

| Process | Threads early / late | Descriptors early / late | Median RSS early / late |
|---|---|---|---|
| Clean receiver | 24 / 24 | 6 / 6 | 46.7 / 46.9 MiB |
| Clean sender | 26 / 26 | 6 / 6 | 54.8 / 54.9 MiB |
| Outage receiver | 24 / 24 | 6 / 6 | 49.3 / 50.6 MiB |
| Outage sender | 26 / 26 | 6 / 6 | 56.9 / 58.0 MiB |

"Early" is minutes 2 to 7, "late" the last 5 minutes. Memory grew by at
most 1.3 MiB over 30 minutes against an allowance of 16 MiB. Thirty
minutes cannot rule out slow growth over days.

The soak is not part of the per-push gate. `.github/workflows/soak.yml`
runs it on the hosted x86-64 runner when started by hand; it has not been
run there yet.

## D40. Gap-aware spectrum demonstration

`examples/gap_aware_spectrum.py` provides `gap_aware_fft`: it builds every
FFT window from contiguous samples only, discards the unfinished window at
a `rwsrt.discontinuity` tag and tags the first spectrum after a break.
Tested with synthetic breaks (window positions, and no energy from the
neighbouring stretch within 60 dB) and behind a real Source with an outage.
The live plot (`--gui`) needs Qt and a display and has not been run.

## Open items carried into M4

1. Network qualification with netem: profiles N0 to N6, UDP and TCP
   baselines, clock accounting. Needs `NET_ADMIN` and the `sch_netem`
   kernel module in the test environment; not yet checked on the hosted
   runner.
2. A dedicated x86-64 host for timing-sensitive results (D2).
3. Overhead reserve (`SRTO_OHEADBW` 25 %) under loss (D20), and records per
   second as the cost driver (D22).
4. The strict Sink's sender-drop path (D33) needs a profile that triggers it.
5. Two-host and hardware experiments.
6. No first user: academic project for now (owner, 2026-11-12).

# Milestone M4: qualification

M4 exit gate (plan section 13): reproducible evidence and an honest
supported operating envelope. Protocol: [benchmarking.md](benchmarking.md).
Results and envelope: [results-m4.md](results-m4.md).

| Plan item | Status |
|---|---|
| netem matrix N0 to N6 | done in emulation: 83 runs, all kept |
| UDP and TCP comparisons with equal framing and deadlines | done |
| Primary envelope declared before collecting results | done; the protocol commit precedes the runs |
| Five 120 s repetitions with warm-up for the headline comparison | done for N0 to N2 |
| Clock accounting | same-host shared monotonic clock: done. Two hosts: not done |
| Two-host experiment | **not done**: no second host |
| Hardware or acquisition adapter | **not done** |
| Reference host for timing | **not done**: a shared virtualised machine was used |
| Full matrix on x86-64 | **not done**: CI runs only a short harness check |

M4 is therefore complete as far as one emulated machine allows, and open
where the plan needs a second host, hardware or a dedicated machine. Field
readiness is not claimed.

## D41. Measurement design

- Three network namespaces per run with netem in the router namespace
  only (`tests/netem/topology.sh`). Nothing outside the container is
  touched.
- Sender and receiver share one monotonic clock. The sender's sample clock
  defines the nominal capture time of every sample; the receiver logs the
  output time of every record. Age is exact up to pacing granularity of
  about 1 ms. No protocol field was misused to carry the time.
- "Usable on time" counts 1024-sample windows delivered completely within
  the deadline (latency + 150 ms): what a block-processing consumer can use.
- Verdicts are derived from recorded evidence by one module used by both
  the runner and the validator. Missing evidence is a failure.

## D42. Baselines

UDP with a fixed playout delay equal to the SRT latency, and TCP with a
send buffer of one latency budget. Both decisions favour the baselines:
without the playout delay UDP could not reorder, and with the kernel's
default buffer TCP delivered data 25 s old in a harness check. All three
transports run in plaintext. Two comparator defects were found during the
runs and are recorded in benchmarking.md; neither changed what was
delivered.

## D43. What the results say

- The SRT gates of the declared envelope passed in 15 of 15 runs.
- The predeclared benefit criterion was met on N2 and not met against TCP
  on N1.
- SRT's cost is latency: its age is the configured latency plus the path
  delay, about ten times TCP's on a mildly impaired path.
- These are properties of SRT with this module's settings in this
  emulation. They are not yet a statement about any real path.

## D44. Limitation found: no bound on age under queueing congestion

With the forward path constrained below the load in front of a deep queue
(N4), nothing is lost, so SRT neither retransmits nor drops, and samples
arrive up to 6.3 s old. 83.7 % were on time. The module passed its N4 gate,
which only asks for bounded memory and no deadlock; the result is still a
limitation a user must know. With a queue of about 250 ms (N4b) the network
drops the excess and SRT stays on time.

Not fixed. Options for later, each a design decision: let the Source
discard records older than a limit when capture times are present; expose
the SRT sender buffer delay and react to it; or document the requirement
of a bounded bottleneck queue. D20's question about the overhead reserve
was not the limiting factor in any profile here.

## D45. Hosted runner

`sch_netem` and privileged containers were first tried on the hosted
runner with this milestone's CI job (`netem-smoke` in `gate.yml`): three
profiles, three transports, 30 s each, validated like the real runs. It
checks the harness. Its figures are not qualification data: the runner has
two shared CPUs. First attempt (run 43271080084): netem loaded and all
nine runs validated; the job then failed on a file-permission error in the
workflow's validation step, which now runs inside the image. Second attempt
(run 43276175175 on commit `c76b5cd`): both jobs pass.

## Open items carried into M5

1. Two-host experiment with independent clocks, and a clock-offset
   procedure for it.
2. Acquisition hardware and an adapter that supplies capture times and
   overrun reports.
3. A dedicated host, and the matrix on x86-64.
4. Bound on end-to-end age under queueing congestion (D44).
5. Repetitions for the secondary cells that matter (1 MS/s, burst loss).
6. The blocks themselves under netem; encryption under impairment.
7. The strict Sink's sender-drop path (D33) was not triggered by any
   profile either.
8. No first user: academic project for now (owner, 2026-11-12).

# Milestone M5: developer release candidate

Status against the plan's M5 row ("Documentation, packaging, clean install,
hardware/acquisition adapter trial; v0.1 release candidate and
external-user reproduction"):

| Item | State |
|---|---|
| Documentation | done: install guide, limitations, compatibility statement, changelog, release checklist; block reference extended |
| Packaging | done: source package from one commit, reproducible, with checksum (D46) |
| Clean install | done: the install guide is executed in a fresh container, locally and in CI (D47) |
| v0.1 release candidate | prepared as commit and package `0.1.0-rc1`. **Not tagged, not published**: owner decision |
| External-user reproduction | **not done**: no person outside the project. An AI agent stood in (D48) |
| Hardware or acquisition adapter trial | **not done**: no hardware |

M5 is therefore complete as far as it can be without hardware and without
an outside user. The state of every item of the plan's definition of done is
in [release-checklist.md](release-checklist.md).

## D46. Version and source package

- The release name is set once, `RWSRT_RELEASE_VERSION` in the top-level
  `CMakeLists.txt` (`0.1.0-rc1`). The blocks report it as
  `versions.module`; the package is named after it.
- The package is `git archive` of one commit, gzip without timestamp: the
  same commit gives the same bytes. `tools/release/make-source-package.sh`
  refuses a dirty tree unless told to package the working tree for a test,
  and then marks the result as not a release.
- No binary package. The plan allows them only for qualified platform
  combinations; one source build path is enough for a first candidate and
  avoids the distribution obligations of D12.
- A release candidate is a commit and a package. No tag was created. No
  workflow in this repository reacts to tags.

## D47. The install guide is the test

`tools/release/check-clean-install.sh` extracts every `sh` block from
`docs/install.md` inside the package and runs them in order in a fresh
container of the pinned base image, as an unprivileged user. It then checks
that the first transfer printed exactly the guide's "Expected output" and
that the installed library links exactly one SRT implementation. The guide
cannot drift from what works without the check failing. Robotweax SRT is
cloned from its public repository at tag `v0.3.2`, and the guide verifies
the pinned commit.

Installed below `~/rwsrt`; environment through one sourced file. An
install into `/usr/local` was not tried.

## D48. Newcomer trial by an AI agent

Plan section 15 asks for one independent developer who reproduces the
examples from the documentation. There is none. As the nearest available
thing, a second AI agent with no knowledge of the project got only the
package and tasks (install, run the examples, write its own f32 sender and
receiver from the documentation, provoke two mistakes). It succeeded on
each and found 18 documentation problems, all addressed
([results-m5.md](results-m5.md)). This does not close the item: same kind
of reader as the author, same machine, no human judgement.

Findings about the software, not the documents, from that trial:

- A Listener cannot see a Caller with a wrong passphrase. SRT rejects it
  before the listen callback, `rejected_peers` stays 0, and the Listener
  ends with `startup_timeout`. Documented; not changed.
- A Sink is not told why the Source refused its stream (rate or format
  mismatch). Strict: `connection_lost`. Segmented: it retries and ends with
  `completed` false and `failure` none. Documented; a rejection reason
  carried back to the Sink would be the fix and is not in this release.

## D49. The blocks under netem, with encryption

Closes open items 6 of M4 as a functional check
([results-m5.md](results-m5.md)): `srt_sink` and `srt_source`, AES-CTR,
segmented, N0 to N2, three runs of 120 s each. Final series 9 of 9: every
sample delivered or reported. Not a timing measurement.

Two behaviours were quantified for the first time:

- Late join on a delayed path (41 ms of signal on N1, 123 to 164 ms on N2,
  once 1.19 s): a segmented Sink does not wait for the handshake.
- Lost END (1 of 6 runs of 120 s on N2): the Source ends only after its
  60 s reconnect budget, with `termination_unknown`. END is one packet with
  nothing after it, so SRT's NAK-based recovery cannot notice its loss.
  Protocol version 1 says END is neither acknowledged nor repeated (D34)
  and is frozen. Options for a later version, not decided: send END more
  than once; let the Source end sooner when the peer closed in an orderly
  way after the last expected sample index.

The first series had one failed run, caused by the check's own time limit;
it is kept.

## D50. Housekeeping

- A fuzz input from M1 had been committed in the repository root by
  mistake. It is now a regression input that the native checks run before
  fuzzing (`tests/fuzz/regressions/`).
- `dependencies.lock.json` lists x86-64 as qualified, with what that
  qualification consists of.
- `THIRD_PARTY_NOTICES.md` was re-checked against the libraries an
  installed module loads; the release check records that list.
- CI has two more jobs: `release-check` and `blocks-netem`.

## Open items after M5

1. Two-host experiment with independent clocks, and a clock-offset
   procedure for it.
2. Acquisition hardware and an adapter that supplies capture times and
   overrun reports.
3. A dedicated host, and the full matrix on x86-64.
4. An independent person reproducing the examples from the documentation.
5. Bound on end-to-end age under queueing congestion (D44).
6. END can be lost and then costs the Source its reconnect budget (D49).
7. A Sink is not told why its stream was refused; a Listener does not see
   a wrong passphrase (D48).
8. Repetitions for the secondary cells of M4 (1 MS/s, burst loss).
9. The strict Sink's sender-drop path (D33) was never triggered.
10. Tag and publication of 0.1.0-rc1, and whether 0.1.0 is released with
    the items above open: owner decisions.
11. No first user: academic project for now (owner, 2026-11-12).

