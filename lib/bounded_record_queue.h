/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_BOUNDED_RECORD_QUEUE_H
#define INCLUDED_RWSRT_BOUNDED_RECORD_QUEUE_H

// Bounded FIFO of complete application records (plan section 9, "Queue
// rules"). All three limits apply: bytes, record count and age.

#include "continuity.h"
#include "wire_codec.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace rwsrt {

struct queue_limits {
    std::size_t max_bytes = 4u * 1024u * 1024u; //!< serialized DATA bytes
    std::size_t max_records = 8192;             //!< DATA records
    std::chrono::nanoseconds max_age = std::chrono::milliseconds(100);
};

enum class overflow_policy {
    fail,       //!< strict mode: a full queue or an expired record is an error
    drop_oldest //!< segmented mode: discard oldest DATA and account for it
};

struct queue_entry {
    std::vector<std::uint8_t> message; //!< one complete serialized record
    record_header header;
    std::optional<discontinuity> brk;  //!< break that precedes this record
    std::chrono::steady_clock::time_point enqueued{};
};

struct drop_counters {
    std::uint64_t records = 0;
    std::uint64_t samples = 0;
    std::uint64_t bytes = 0;
};

struct queue_stats {
    std::size_t records = 0;
    std::size_t bytes = 0;
    std::size_t high_water_records = 0;
    std::size_t high_water_bytes = 0;
    drop_counters dropped_overflow;
    drop_counters dropped_expired;
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
};

enum class push_status {
    ok,
    overflow,  //!< fail policy: limits reached, record not stored
    too_large, //!< a single record exceeds max_bytes
    after_end, //!< END is already queued; nothing may follow it
    closed
};

enum class pop_status {
    record,
    empty,
    expired, //!< fail policy: the oldest record outlived max_age
    closed   //!< closed and drained
};

/*!
 * Thread-safe. END has reserved capacity: it is always admitted, is never
 * evicted and never expires, and because it is appended like any record it
 * can neither evict nor overtake DATA.
 *
 * With \p track_breaks (receiver side), samples discarded here are reported
 * on the next record that leaves the queue as a discontinuity carrying
 * reason_receiver_queue_drop, merged with any break the discarded records
 * carried themselves. Nothing is lost from continuity accounting.
 */
class bounded_record_queue
{
public:
    using clock = std::chrono::steady_clock;

    bounded_record_queue(queue_limits limits, overflow_policy policy, bool track_breaks);

    push_status push(queue_entry&& entry, clock::time_point now);

    //! Non-blocking. Applies the age limit first.
    pop_status pop(queue_entry& out, clock::time_point now);

    //! Waits up to \p timeout for a record, the close, or an expiry error.
    pop_status wait_pop(queue_entry& out, std::chrono::nanoseconds timeout);

    //! Applies the age limit without removing a live record. Returns false
    //! when the fail policy found an expired record.
    bool expire(clock::time_point now);

    //! Rejects further pushes and wakes every waiter. Queued records stay
    //! poppable so a natural end can drain; discard() drops them instead.
    void close();
    //! Removes everything that is queued, without drop accounting.
    void discard();
    bool closed() const;

    queue_stats stats() const;

private:
    struct pending_drop {
        discontinuity brk;
    };

    void evict_front_locked(drop_counters& counters);
    bool expire_locked(clock::time_point now);
    pop_status pop_locked(queue_entry& out, clock::time_point now);

    const queue_limits d_limits;
    const overflow_policy d_policy;
    const bool d_track_breaks;

    mutable std::mutex d_mutex;
    std::condition_variable d_available;
    std::deque<queue_entry> d_entries;
    std::size_t d_data_bytes = 0;
    std::size_t d_data_records = 0;
    bool d_end_queued = false;
    bool d_closed = false;
    std::optional<pending_drop> d_pending_drop;
    queue_stats d_stats;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_BOUNDED_RECORD_QUEUE_H */
