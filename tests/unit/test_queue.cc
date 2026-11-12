// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.h"

#include <atomic>
#include <thread>

using namespace rwsrt;
using namespace rwsrt_test;
using namespace std::chrono_literals;

namespace {

using clock_type = bounded_record_queue::clock;
const clock_type::time_point t0 = clock_type::time_point() + 1h;

// 10 cf32 samples: 96 + 80 = 176 bytes per record.
constexpr std::uint32_t n = 10;
constexpr std::size_t record_bytes = 176;

queue_entry rec(std::uint64_t index, std::uint64_t seq)
{
    return entry_of(data_header(0, index * n, n, seq));
}

queue_limits limits(std::size_t records, std::size_t bytes = 1u << 20,
                    std::chrono::nanoseconds age = 1h)
{
    return queue_limits{ bytes, records, age };
}

} // namespace

BOOST_AUTO_TEST_SUITE(bounded_queue)

BOOST_AUTO_TEST_CASE(fifo_order_and_stats)
{
    bounded_record_queue q(limits(8), overflow_policy::fail, false);
    for (std::uint64_t i = 0; i < 3; ++i)
        BOOST_TEST((q.push(rec(i, i), t0) == push_status::ok));
    auto s = q.stats();
    BOOST_TEST(s.records == 3u);
    BOOST_TEST(s.bytes == 3 * record_bytes);
    queue_entry e;
    for (std::uint64_t i = 0; i < 3; ++i) {
        BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
        BOOST_TEST(e.header.record_sequence == i);
        BOOST_TEST(!e.brk.has_value());
    }
    BOOST_TEST((q.pop(e, t0) == pop_status::empty));
    s = q.stats();
    BOOST_TEST(s.bytes == 0u);
    BOOST_TEST(s.high_water_records == 3u);
    BOOST_TEST(s.high_water_bytes == 3 * record_bytes);
    BOOST_TEST(s.pushed == 3u);
    BOOST_TEST(s.popped == 3u);
}

BOOST_AUTO_TEST_CASE(strict_policy_fails_on_each_limit)
{
    {
        bounded_record_queue q(limits(2), overflow_policy::fail, false); // record count
        BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
        BOOST_TEST((q.push(rec(1, 1), t0) == push_status::ok));
        BOOST_TEST((q.push(rec(2, 2), t0) == push_status::overflow));
        BOOST_TEST(q.stats().records == 2u); // nothing was displaced
        BOOST_TEST(q.stats().dropped_overflow.records == 0u);
    }
    {
        bounded_record_queue q(limits(100, 2 * record_bytes + 1), overflow_policy::fail, false);
        BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
        BOOST_TEST((q.push(rec(1, 1), t0) == push_status::ok));
        BOOST_TEST((q.push(rec(2, 2), t0) == push_status::overflow)); // bytes
    }
    {
        bounded_record_queue q(limits(100, 1u << 20, 100ms), overflow_policy::fail, false);
        BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
        queue_entry e;
        BOOST_TEST(q.expire(t0 + 100ms)); // exactly at the limit is still live
        BOOST_TEST(!q.expire(t0 + 101ms));
        BOOST_TEST((q.pop(e, t0 + 101ms) == pop_status::expired)); // age
        BOOST_TEST(q.stats().records == 1u); // strict never discards silently
    }
    {
        bounded_record_queue q(limits(100, record_bytes - 1), overflow_policy::fail, false);
        BOOST_TEST((q.push(rec(0, 0), t0) == push_status::too_large));
    }
}

BOOST_AUTO_TEST_CASE(drop_oldest_accounts_for_every_discarded_sample)
{
    bounded_record_queue q(limits(3), overflow_policy::drop_oldest, true);
    for (std::uint64_t i = 0; i < 5; ++i) // records 0 and 1 are displaced
        BOOST_TEST((q.push(rec(i, i), t0) == push_status::ok));
    auto s = q.stats();
    BOOST_TEST(s.records == 3u);
    BOOST_TEST(s.dropped_overflow.records == 2u);
    BOOST_TEST(s.dropped_overflow.samples == 2u * n);
    BOOST_TEST(s.dropped_overflow.bytes == 2 * record_bytes);

    queue_entry e;
    BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
    BOOST_TEST(e.header.record_sequence == 2u);
    // The surviving record reports exactly what was discarded before it.
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->reasons == reason_receiver_queue_drop);
    BOOST_TEST(e.brk->missing_known);
    BOOST_TEST(e.brk->missing_samples == 2u * n);
    BOOST_TEST(e.brk->has_previous);
    BOOST_TEST(e.brk->previous.sample_index == 0u);
    BOOST_TEST(e.brk->next.sample_index == 2u * n);
    // Reported once only.
    BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
    BOOST_TEST(!e.brk.has_value());
}

BOOST_AUTO_TEST_CASE(drop_merges_breaks_carried_by_discarded_records)
{
    bounded_record_queue q(limits(2), overflow_policy::drop_oldest, true);
    // Record A followed a known network gap of 30 samples.
    queue_entry a = entry_of(data_header(0, 30, n, 3));
    discontinuity gap;
    gap.reasons = reason_sample_gap;
    gap.has_previous = true;
    gap.previous = coordinate{ run_of(0xa0), 0, 0 };
    gap.next = coordinate{ run_of(0xa0), 0, 30 };
    gap.missing_samples = 30;
    gap.connection_generation = 1;
    a.brk = gap;
    BOOST_TEST((q.push(std::move(a), t0) == push_status::ok));
    BOOST_TEST((q.push(entry_of(data_header(0, 40, n, 4)), t0) == push_status::ok));
    // Record C followed an unknown upstream gap; pushing it displaces A.
    queue_entry c = entry_of(data_header(1, 0, n, 5));
    discontinuity unknown;
    unknown.reasons = reason_upstream_unknown_gap;
    unknown.has_previous = true;
    unknown.previous = coordinate{ run_of(0xa0), 0, 50 };
    unknown.next = coordinate{ run_of(0xa0), 1, 0 };
    unknown.missing_known = false;
    unknown.connection_generation = 1;
    c.brk = unknown;
    BOOST_TEST((q.push(std::move(c), t0) == push_status::ok));

    queue_entry e;
    BOOST_REQUIRE((q.pop(e, t0) == pop_status::record)); // B, after dropped A
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->reasons == (reason_receiver_queue_drop | reason_sample_gap));
    BOOST_TEST(e.brk->missing_known);
    BOOST_TEST(e.brk->missing_samples == 30u + n); // network gap + discarded record
    BOOST_TEST(e.brk->previous.sample_index == 0u); // where the earlier break began
    BOOST_TEST(e.brk->next.sample_index == 40u);
    BOOST_REQUIRE((q.pop(e, t0) == pop_status::record)); // C keeps its own break
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->reasons == reason_upstream_unknown_gap);

    // A drop directly before a record with an unknown gap: unknown wins.
    bounded_record_queue q2(limits(1), overflow_policy::drop_oldest, true);
    BOOST_TEST((q2.push(entry_of(data_header(0, 0, n, 0)), t0) == push_status::ok));
    queue_entry c2 = entry_of(data_header(1, 0, n, 1));
    c2.brk = unknown;
    BOOST_TEST((q2.push(std::move(c2), t0) == push_status::ok));
    BOOST_REQUIRE((q2.pop(e, t0) == pop_status::record));
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->reasons == (reason_receiver_queue_drop | reason_upstream_unknown_gap));
    BOOST_TEST(!e.brk->missing_known);
    BOOST_TEST(e.brk->missing_samples == 0u);
}

BOOST_AUTO_TEST_CASE(expired_records_are_discarded_not_delivered_late)
{
    bounded_record_queue q(limits(100, 1u << 20, 100ms), overflow_policy::drop_oldest, true);
    BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
    BOOST_TEST((q.push(rec(1, 1), t0 + 50ms) == push_status::ok));
    BOOST_TEST((q.push(rec(2, 2), t0 + 120ms) == push_status::ok)); // expires record 0
    BOOST_TEST(q.stats().dropped_expired.records == 1u);
    queue_entry e;
    BOOST_REQUIRE((q.pop(e, t0 + 160ms) == pop_status::record)); // record 1 expired too
    BOOST_TEST(e.header.record_sequence == 2u);
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->missing_samples == 2u * n);
    const auto s = q.stats();
    BOOST_TEST(s.dropped_expired.records == 2u);
    BOOST_TEST(s.dropped_overflow.records == 0u);
    // A whole backlog can expire; the queue is then simply empty.
    BOOST_TEST((q.push(rec(3, 3), t0 + 200ms) == push_status::ok));
    BOOST_TEST((q.pop(e, t0 + 10s) == pop_status::empty));
    BOOST_TEST(q.stats().dropped_expired.records == 3u);
    // The next record still reports the discarded one.
    BOOST_TEST((q.push(rec(4, 4), t0 + 10s) == push_status::ok));
    BOOST_REQUIRE((q.pop(e, t0 + 10s) == pop_status::record));
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->missing_samples == n);
}

BOOST_AUTO_TEST_CASE(sender_side_queue_does_not_attach_breaks)
{
    bounded_record_queue q(limits(1), overflow_policy::drop_oldest, false);
    BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
    BOOST_TEST((q.push(rec(1, 1), t0) == push_status::ok));
    queue_entry e;
    BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
    BOOST_TEST(!e.brk.has_value());
    BOOST_TEST(q.stats().dropped_overflow.samples == n);
}

BOOST_AUTO_TEST_CASE(end_has_reserved_capacity_and_keeps_order)
{
    for (auto policy : { overflow_policy::fail, overflow_policy::drop_oldest }) {
        bounded_record_queue q(limits(2, 1u << 20, 100ms), policy, true);
        BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
        BOOST_TEST((q.push(rec(1, 1), t0) == push_status::ok));
        // The queue is full, yet END is admitted and displaces nothing.
        BOOST_TEST((q.push(entry_of(end_header(0, 2 * n, 2)), t0) == push_status::ok));
        BOOST_TEST(q.stats().records == 3u);
        BOOST_TEST(q.stats().dropped_overflow.records == 0u);
        // Nothing may follow END.
        BOOST_TEST((q.push(rec(2, 3), t0) == push_status::after_end));
        BOOST_TEST((q.push(entry_of(end_header(0, 2 * n, 3)), t0) == push_status::after_end));
        queue_entry e;
        BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
        BOOST_TEST(e.header.record_sequence == 0u);
        BOOST_REQUIRE((q.pop(e, t0) == pop_status::record));
        BOOST_TEST(e.header.record_sequence == 1u);
        BOOST_REQUIRE((q.pop(e, t0) == pop_status::record)); // END last
        BOOST_TEST((e.header.kind == record_kind::end));
    }
    // END never expires; DATA discarded before it is reported on it.
    bounded_record_queue q(limits(2, 1u << 20, 100ms), overflow_policy::drop_oldest, true);
    BOOST_TEST((q.push(rec(0, 0), t0) == push_status::ok));
    BOOST_TEST((q.push(entry_of(end_header(0, n, 1)), t0) == push_status::ok));
    queue_entry e;
    BOOST_REQUIRE((q.pop(e, t0 + 1h) == pop_status::record));
    BOOST_TEST((e.header.kind == record_kind::end));
    BOOST_REQUIRE(e.brk.has_value());
    BOOST_TEST(e.brk->reasons == reason_receiver_queue_drop);
    BOOST_TEST(e.brk->missing_samples == n);
}

BOOST_AUTO_TEST_CASE(close_wakes_waiters_and_drains)
{
    bounded_record_queue q(limits(8), overflow_policy::fail, false);
    std::atomic<int> result{ -1 };
    std::thread waiter([&] {
        queue_entry e;
        result = static_cast<int>(q.wait_pop(e, 10s));
    });
    std::this_thread::sleep_for(30ms);
    const auto before = clock_type::now();
    q.close();
    waiter.join();
    BOOST_TEST((clock_type::now() - before < 1s));
    BOOST_TEST(result.load() == static_cast<int>(pop_status::closed));
    BOOST_TEST((q.push(rec(0, 0), t0) == push_status::closed));

    // Records queued before the close remain poppable; discard() drops them.
    bounded_record_queue q2(limits(8), overflow_policy::fail, false);
    BOOST_TEST((q2.push(rec(0, 0), clock_type::now()) == push_status::ok));
    BOOST_TEST((q2.push(rec(1, 1), clock_type::now()) == push_status::ok));
    q2.close();
    queue_entry e;
    BOOST_TEST((q2.wait_pop(e, 1ms) == pop_status::record));
    q2.discard();
    BOOST_TEST((q2.wait_pop(e, 1ms) == pop_status::closed));
}

BOOST_AUTO_TEST_CASE(wait_pop_times_out_and_receives)
{
    bounded_record_queue q(limits(8), overflow_policy::fail, false);
    queue_entry e;
    const auto before = clock_type::now();
    BOOST_TEST((q.wait_pop(e, 20ms) == pop_status::empty));
    BOOST_TEST((clock_type::now() - before >= 20ms));
    // Built here: Boost.Test assertions (inside rec) are not thread-safe.
    queue_entry late = rec(7, 7);
    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        (void)q.push(std::move(late), clock_type::now());
    });
    BOOST_TEST((q.wait_pop(e, 10s) == pop_status::record));
    BOOST_TEST(e.header.record_sequence == 7u);
    producer.join();
}

BOOST_AUTO_TEST_CASE(concurrent_producer_consumer_conserves_samples)
{
    // Under drop-oldest, every produced sample is either delivered or
    // reported missing on a delivered record. Run under TSan as well.
    bounded_record_queue q(limits(16), overflow_policy::drop_oldest, true);
    constexpr std::uint64_t total = 20000;
    std::uint64_t delivered = 0, reported_missing = 0, next_expected = 0;
    bool ordered = true;
    std::thread consumer([&] {
        queue_entry e;
        for (;;) {
            const pop_status s = q.wait_pop(e, 50ms);
            if (s == pop_status::closed)
                break;
            if (s != pop_status::record)
                continue;
            if (e.brk)
                reported_missing += e.brk->missing_samples;
            ordered = ordered && e.header.first_sample_index ==
                                     next_expected + (e.brk ? e.brk->missing_samples : 0);
            next_expected = e.header.first_sample_index + e.header.sample_count;
            delivered += e.header.sample_count;
        }
    });
    for (std::uint64_t i = 0; i < total; ++i)
        BOOST_REQUIRE((q.push(rec(i, i), clock_type::now()) == push_status::ok));
    // Let the consumer drain, then close.
    while (q.stats().records != 0)
        std::this_thread::sleep_for(1ms);
    q.close();
    consumer.join();
    const auto s = q.stats();
    BOOST_TEST(ordered);
    BOOST_TEST(delivered + s.dropped_overflow.samples == total * n);
    // Drops after the last delivered record have no record to report on.
    BOOST_TEST(reported_missing <= s.dropped_overflow.samples);
    BOOST_TEST(next_expected == total * n);
    BOOST_TEST(reported_missing == s.dropped_overflow.samples);
    BOOST_TEST(s.high_water_records <= 16u);
}

BOOST_AUTO_TEST_SUITE_END()
