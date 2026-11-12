/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m0_probe_source_impl.h"

#include <gnuradio/io_signature.h>

#include <boost/thread/thread.hpp>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace gr {
namespace robotweax_srt {

namespace {
constexpr int empty_queue_wait_ms = 20; // plan section 9
double ms_between(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}
} // namespace

m0_probe_source::sptr m0_probe_source::make(int finish_after_ms, bool wait_inside_work)
{
    return gnuradio::make_block_sptr<m0_probe_source_impl>(finish_after_ms, wait_inside_work);
}

m0_probe_source_impl::m0_probe_source_impl(int finish_after_ms, bool wait_inside_work)
    : gr::sync_block("m0_probe_source",
                     gr::io_signature::make(0, 0, 0),
                     gr::io_signature::make(1, 1, sizeof(gr_complex))),
      d_finish_after_ms(finish_after_ms),
      d_wait_inside_work(wait_inside_work)
{
}

m0_probe_source_impl::~m0_probe_source_impl() {}

bool m0_probe_source_impl::start()
{
    std::lock_guard<std::mutex> guard(d_mutex);
    ++d_start_calls;
    d_cancel = false;
    d_running = true;
    d_started = steady::now();
    d_last_call = d_started;
    return sync_block::start();
}

bool m0_probe_source_impl::stop()
{
    const auto t0 = steady::now();
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        ++d_stop_calls;
        d_cancel = true;
        if (d_running) {
            d_running = false;
            d_run_ms = ms_between(d_started, t0);
        }
    }
    d_wake.notify_all();
    const bool ok = sync_block::stop();
    std::lock_guard<std::mutex> guard(d_mutex);
    d_stop_ms = ms_between(t0, steady::now());
    return ok;
}

void m0_probe_source_impl::inject(int items)
{
    if (items <= 0)
        return;
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        if (d_pending_items == 0) {
            d_injected_at = steady::now();
            d_latency_armed = true;
        }
        d_pending_items += items;
    }
    d_wake.notify_all();
}

int m0_probe_source_impl::work(int noutput_items,
                               gr_vector_const_void_star& /*input_items*/,
                               gr_vector_void_star& output_items)
{
    std::unique_lock<std::mutex> lock(d_mutex);
    const auto now = steady::now();
    if (d_work_calls > 0) {
        const double gap = ms_between(d_last_call, now);
        d_min_gap_ms = d_min_gap_ms < 0 ? gap : std::min(d_min_gap_ms, gap);
        d_max_gap_ms = std::max(d_max_gap_ms, gap);
    }
    d_last_call = now;
    ++d_work_calls;

    for (;;) {
        if (d_pending_items > 0) {
            const int n = static_cast<int>(
                std::min<std::int64_t>(d_pending_items, noutput_items));
            std::memset(output_items[0], 0, sizeof(gr_complex) * n);
            d_pending_items -= n;
            d_items_produced += static_cast<std::uint64_t>(n);
            if (d_latency_armed) {
                const double ms = ms_between(d_injected_at, steady::now());
                d_min_delivery_ms = d_min_delivery_ms < 0 ? ms : std::min(d_min_delivery_ms, ms);
                d_max_delivery_ms = std::max(d_max_delivery_ms, ms);
                ++d_deliveries;
                d_latency_armed = false;
            }
            return n;
        }
        if (d_finish_after_ms > 0 &&
            ms_between(d_started, steady::now()) >= d_finish_after_ms) {
            d_returned_done = true;
            return WORK_DONE;
        }
        if (d_cancel)
            return 0;
        // The scheduler thread is a boost thread and top_block::stop()
        // interrupts it. A std::condition_variable wait is not a boost
        // interruption point, so the request is polled between slices.
        if (boost::this_thread::interruption_requested()) {
            ++d_interruptions_seen;
            return 0; // the scheduler loop then honours the interruption
        }
        d_wake.wait_for(lock, std::chrono::milliseconds(empty_queue_wait_ms));
        if (!d_wait_inside_work && d_pending_items == 0)
            return 0;
    }
}

std::string m0_probe_source_impl::report() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    std::ostringstream os;
    os << "{\"wait_inside_work\":" << (d_wait_inside_work ? "true" : "false")
       << ",\"start_calls\":" << d_start_calls << ",\"stop_calls\":" << d_stop_calls
       << ",\"work_calls\":" << d_work_calls << ",\"run_ms\":" << d_run_ms
       << ",\"min_call_gap_ms\":" << d_min_gap_ms << ",\"max_call_gap_ms\":" << d_max_gap_ms
       << ",\"stop_ms\":" << d_stop_ms << ",\"returned_work_done\":"
       << (d_returned_done ? "true" : "false") << ",\"interruptions_seen\":"
       << d_interruptions_seen << ",\"items_produced\":" << d_items_produced
       << ",\"deliveries\":" << d_deliveries << ",\"min_delivery_ms\":" << d_min_delivery_ms
       << ",\"max_delivery_ms\":" << d_max_delivery_ms << "}";
    return os.str();
}

} /* namespace robotweax_srt */
} /* namespace gr */
