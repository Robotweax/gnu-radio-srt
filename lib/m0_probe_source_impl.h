/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_IMPL_H
#define INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_IMPL_H

#include <gnuradio/robotweax_srt/m0_probe_source.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gr {
namespace robotweax_srt {

class m0_probe_source_impl : public m0_probe_source
{
private:
    using steady = std::chrono::steady_clock;

    const int d_finish_after_ms;
    const bool d_wait_inside_work;

    mutable std::mutex d_mutex;
    std::condition_variable d_wake;
    bool d_cancel = false;
    bool d_running = false;
    steady::time_point d_started;
    steady::time_point d_last_call;
    std::uint64_t d_work_calls = 0;
    double d_min_gap_ms = -1.0;
    double d_max_gap_ms = 0.0;
    double d_run_ms = 0.0;
    double d_stop_ms = 0.0;
    int d_start_calls = 0;
    int d_stop_calls = 0;
    bool d_returned_done = false;
    std::uint64_t d_interruptions_seen = 0;

    // The stand-in queue.
    std::int64_t d_pending_items = 0;
    steady::time_point d_injected_at;
    bool d_latency_armed = false;
    std::uint64_t d_items_produced = 0;
    std::uint64_t d_deliveries = 0;
    double d_min_delivery_ms = -1.0;
    double d_max_delivery_ms = 0.0;

public:
    m0_probe_source_impl(int finish_after_ms, bool wait_inside_work);
    ~m0_probe_source_impl() override;

    bool start() override;
    bool stop() override;
    std::string report() const override;
    void inject(int items) override;

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_IMPL_H */
