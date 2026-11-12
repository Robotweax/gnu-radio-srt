// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz target: arbitrary sequences of records and connection events through
// the continuity ledger and the receive queue, and arbitrary event sequences
// through the session state machine. Checks the invariants that the rest of
// the module relies on.

#include "bounded_record_queue.h"
#include "continuity.h"
#include "session_state.h"

#include <cstdlib>

namespace {

using namespace rwsrt;

struct reader {
    const std::uint8_t* p;
    std::size_t n;
    bool empty() const { return n == 0; }
    std::uint8_t byte()
    {
        if (n == 0)
            return 0;
        --n;
        return *p++;
    }
};

void check(bool ok)
{
    if (!ok)
        std::abort();
}

void fuzz_ledger(reader in, continuity_policy policy)
{
    continuity_ledger ledger(policy, sample_format::cf32, 100000, 1);
    bounded_record_queue queue(queue_limits{ 2000, 4, std::chrono::hours(1) },
                               overflow_policy::drop_oldest, true);
    const auto now = bounded_record_queue::clock::time_point() + std::chrono::hours(1);
    ledger.on_connection();

    // Small alphabets keep the fuzzer near the interesting boundaries.
    std::uint64_t accepted_samples = 0, queued_out = 0;
    bool was_failed = false;
    while (!in.empty()) {
        const std::uint8_t op = in.byte();
        if ((op & 0x0f) == 0x0f) {
            ledger.on_connection();
            continue;
        }
        record_header h;
        h.kind = (op & 0x10) ? record_kind::end : record_kind::data;
        h.format = (op & 0x80) && (op & 0x40) ? sample_format::f32 : sample_format::cf32;
        h.run_id[0] = static_cast<std::uint8_t>((op >> 5) & 1);
        h.segment_id = in.byte() & 0x03;
        h.first_sample_index = in.byte() & 0x3f;
        h.sample_count = h.kind == record_kind::data ? 1u + (in.byte() & 0x07) : 0u;
        h.rate_num = 100000;
        h.rate_den = 1;
        h.record_sequence = in.byte() & 0x1f;
        if (validate(h) != wire_error::none)
            continue;

        const auto before = ledger.expectation();
        const ledger_result r = ledger.on_record(h);
        check(r.accepted == (r.failure == ledger_failure::none));
        if (was_failed)
            check(!r.accepted); // failure is sticky
        was_failed = ledger.failed();
        if (!r.accepted) {
            check(ledger.failed());
            check(ledger.expectation() == before); // a rejected record changes nothing
            continue;
        }
        if (policy == continuity_policy::strict_stop)
            check(!r.brk.has_value()); // strict never passes a break
        check(ledger.expectation().has_value());
        check(ledger.expectation()->sample_index == h.first_sample_index + h.sample_count);
        check(r.is_end == (h.kind == record_kind::end));
        // A run observed from its clean start to END is complete even when
        // it began at a new_run boundary; any other break rules it out.
        if (r.end_complete)
            check(r.is_end &&
                  (!r.brk || (r.brk->reasons & ~(reason_new_run | reason_reconnect)) == 0));
        if (r.brk)
            check(r.brk->reasons != 0 && r.brk->next.sample_index == h.first_sample_index);
        accepted_samples += h.sample_count;

        // Accepted records flow through a tiny drop-oldest queue.
        queue_entry e;
        e.header = h;
        e.message.assign(header_size + h.sample_count * 8u, 0);
        e.brk = r.brk;
        const push_status ps = queue.push(std::move(e), now);
        check(ps == push_status::ok || ps == push_status::after_end);
        if (ps == push_status::after_end)
            accepted_samples -= h.sample_count; // a new run after a queued END
        if (op & 0x08) {
            queue_entry out;
            while (queue.pop(out, now) == pop_status::record) {
                queued_out += out.header.sample_count;
                // A record that follows a local drop always says so.
                if (out.brk && (out.brk->reasons & reason_receiver_queue_drop))
                    check(out.brk->next.sample_index == out.header.first_sample_index);
            }
        }
    }
    const queue_stats s = queue.stats();
    check(s.records <= 5 && s.bytes <= 2000);
    // Conservation: every accepted sample was delivered, is still queued, or
    // was counted as dropped.
    std::uint64_t still_queued = 0;
    queue_entry out;
    while (queue.pop(out, now) == pop_status::record)
        still_queued += out.header.sample_count;
    check(queued_out + still_queued + s.dropped_overflow.samples == accepted_samples);
}

void fuzz_fsm(reader in)
{
    const std::uint8_t setup = in.byte();
    session_fsm f((setup & 1) ? connection_role::caller : connection_role::listener,
                  (setup & 2) ? continuity_policy::segmented_live : continuity_policy::strict_stop,
                  (setup & 4) != 0);
    while (!in.empty()) {
        const session_state before = f.state();
        const session_failure failure_before = f.failure();
        const std::uint64_t generation_before = f.connection_generation();
        const std::uint8_t op = in.byte();
        bool restarted = false;
        switch (op % 12) {
        case 0: restarted = f.start(); break;
        case 1: f.on_connected(); break;
        case 2: f.on_attempt_failed(attempt_failure::transient, session_failure::none); break;
        case 3: f.on_attempt_failed(attempt_failure::terminal, session_failure::credentials); break;
        case 4: f.on_retry_elapsed(); break;
        case 5: f.on_connection_lost(); break;
        case 6: f.on_end_of_stream(); break;
        case 7: f.on_drained(); break;
        case 8: (op & 0x80) ? f.on_startup_deadline() : f.on_reconnect_budget_exhausted(); break;
        case 9: f.on_fatal(static_cast<session_failure>(1 + (op >> 4) % 10)); break;
        case 10: f.request_stop(); break;
        case 11: f.on_stopped(); break;
        }
        const session_state after = f.state();
        check(after != session_state::starting); // never observable
        if (!restarted) {
            check(f.connection_generation() >= generation_before);
            if (failure_before != session_failure::none)
                check(f.failure() == failure_before); // first failure is retained
        }
        // Terminal states are left only through stop.
        if (before == session_state::failed || before == session_state::completed)
            check(after == before || after == session_state::stopping);
        if (before == session_state::stopping)
            check(after == session_state::stopping || after == session_state::stopped);
        if (after == session_state::failed)
            check(f.failure() != session_failure::none);
        if (after == session_state::completed)
            check(f.failure() == session_failure::none);
        if (after == session_state::connected && before != session_state::connected)
            check(f.connection_generation() == generation_before + 1);
        // Strict mode, or reconnect disabled, never waits after a connection.
        if (after == session_state::retry_wait && f.ever_connected())
            check(f.reconnect_allowed());
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2)
        return 0;
    reader in{ data + 1, size - 1 };
    switch (data[0] % 3) {
    case 0: fuzz_ledger(in, continuity_policy::segmented_live); break;
    case 1: fuzz_ledger(in, continuity_policy::strict_stop); break;
    default: fuzz_fsm(in); break;
    }
    return 0;
}
