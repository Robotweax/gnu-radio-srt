/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bounded_record_queue.h"

#include <algorithm>
#include <utility>

namespace rwsrt {

bounded_record_queue::bounded_record_queue(queue_limits limits,
                                           overflow_policy policy,
                                           bool track_breaks)
    : d_limits(limits), d_policy(policy), d_track_breaks(track_breaks)
{
}

void bounded_record_queue::evict_front_locked(drop_counters& counters)
{
    queue_entry& e = d_entries.front();
    ++counters.records;
    counters.samples += e.header.sample_count;
    counters.bytes += e.message.size();
    d_data_bytes -= e.message.size();
    --d_data_records;

    if (d_track_breaks) {
        if (!d_pending_drop) {
            pending_drop p;
            p.brk.reasons = reason_receiver_queue_drop;
            if (e.brk) {
                // The discarded record itself followed a break: the report
                // starts where that earlier break started.
                p.brk.reasons |= e.brk->reasons;
                p.brk.has_previous = e.brk->has_previous;
                p.brk.previous = e.brk->previous;
                p.brk.missing_known = e.brk->missing_known;
                p.brk.missing_samples = e.brk->missing_samples;
                p.brk.connection_generation = e.brk->connection_generation;
            } else {
                p.brk.has_previous = true;
                p.brk.previous = coordinate{ e.header.run_id, e.header.segment_id,
                                             e.header.first_sample_index };
            }
            d_pending_drop = p;
        } else if (e.brk) {
            d_pending_drop->brk.reasons |= e.brk->reasons;
            d_pending_drop->brk.missing_known =
                d_pending_drop->brk.missing_known && e.brk->missing_known;
            d_pending_drop->brk.missing_samples += e.brk->missing_samples;
            d_pending_drop->brk.connection_generation = e.brk->connection_generation;
        }
        d_pending_drop->brk.missing_samples += e.header.sample_count;
    }
    d_entries.pop_front();
}

bool bounded_record_queue::expire_locked(clock::time_point now)
{
    while (!d_entries.empty()) {
        const queue_entry& e = d_entries.front();
        if (e.header.kind == record_kind::end || now - e.enqueued <= d_limits.max_age)
            break;
        if (d_policy == overflow_policy::fail)
            return false;
        evict_front_locked(d_stats.dropped_expired);
    }
    return true;
}

push_status bounded_record_queue::push(queue_entry&& entry, clock::time_point now)
{
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        if (d_closed)
            return push_status::closed;
        if (d_end_queued)
            return push_status::after_end;
        entry.enqueued = now;
        if (entry.header.kind == record_kind::end) {
            d_end_queued = true; // reserved capacity: not counted against limits
        } else {
            const std::size_t size = entry.message.size();
            if (size > d_limits.max_bytes || d_limits.max_records == 0)
                return push_status::too_large;
            if (d_policy == overflow_policy::drop_oldest)
                (void)expire_locked(now);
            while (d_data_records + 1 > d_limits.max_records ||
                   d_data_bytes + size > d_limits.max_bytes) {
                if (d_policy == overflow_policy::fail)
                    return push_status::overflow;
                evict_front_locked(d_stats.dropped_overflow);
            }
            d_data_bytes += size;
            ++d_data_records;
        }
        d_entries.push_back(std::move(entry));
        ++d_stats.pushed;
        d_stats.high_water_records = std::max(d_stats.high_water_records, d_entries.size());
        d_stats.high_water_bytes = std::max(d_stats.high_water_bytes, d_data_bytes);
    }
    d_available.notify_one();
    return push_status::ok;
}

pop_status bounded_record_queue::pop_locked(queue_entry& out, clock::time_point now)
{
    if (!expire_locked(now))
        return pop_status::expired;
    if (d_entries.empty())
        return d_closed ? pop_status::closed : pop_status::empty;
    out = std::move(d_entries.front());
    d_entries.pop_front();
    if (out.header.kind == record_kind::end) {
        d_end_queued = false;
    } else {
        d_data_bytes -= out.message.size();
        --d_data_records;
    }
    if (d_pending_drop) {
        discontinuity merged = d_pending_drop->brk;
        if (out.brk) {
            merged.reasons |= out.brk->reasons;
            merged.missing_known = merged.missing_known && out.brk->missing_known;
            merged.missing_samples += out.brk->missing_samples;
            merged.connection_generation = out.brk->connection_generation;
        }
        if (!merged.missing_known)
            merged.missing_samples = 0;
        merged.next = coordinate{ out.header.run_id, out.header.segment_id,
                                  out.header.first_sample_index };
        out.brk = merged;
        d_pending_drop.reset();
    }
    ++d_stats.popped;
    return pop_status::record;
}

pop_status bounded_record_queue::pop(queue_entry& out, clock::time_point now)
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return pop_locked(out, now);
}

pop_status bounded_record_queue::wait_pop(queue_entry& out, std::chrono::nanoseconds timeout)
{
    std::unique_lock<std::mutex> lock(d_mutex);
    d_available.wait_for(lock, timeout, [this] { return !d_entries.empty() || d_closed; });
    return pop_locked(out, clock::now());
}

bool bounded_record_queue::expire(clock::time_point now)
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return expire_locked(now);
}

void bounded_record_queue::close()
{
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        d_closed = true;
    }
    d_available.notify_all();
}

void bounded_record_queue::discard()
{
    std::lock_guard<std::mutex> guard(d_mutex);
    d_entries.clear();
    d_data_bytes = 0;
    d_data_records = 0;
    d_end_queued = false;
    d_pending_drop.reset();
}

bool bounded_record_queue::closed() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return d_closed;
}

queue_stats bounded_record_queue::stats() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    queue_stats s = d_stats;
    s.records = d_entries.size();
    s.bytes = d_data_bytes;
    return s;
}

} // namespace rwsrt
