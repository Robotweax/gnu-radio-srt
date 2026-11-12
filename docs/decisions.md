# Decisions and M0 findings

Records what milestone M0 established on the pinned platform, the decisions
taken, and where they amend [project-plan.md](project-plan.md) (v1.0). The
plan file is kept as received; this file wins where the two differ.

Numbers are measurements from one host inside a container. They show that a
mechanism works and roughly how it behaves. They are not performance claims.
Evidence: `tools/dev/run.sh tools/dev/gate.sh`, summaries in
[evidence/m0/](evidence/m0/) and [evidence/m1/](evidence/m1/).

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
7. The thread-count watch item (D6).
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
| x86-64 | see the CI run of the M1 commit | `.github/workflows/gate.yml` |

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
