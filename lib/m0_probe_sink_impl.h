/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_IMPL_H
#define INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_IMPL_H

#include "runtime_owner.h"
#include <gnuradio/robotweax_srt/m0_probe_sink.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gr {
namespace robotweax_srt {

class m0_probe_sink_impl : public m0_probe_sink
{
private:
    const bool d_listener;
    const std::string d_host;
    const int d_port;

    std::shared_ptr<runtime_owner::lease> d_runtime;
    std::thread d_worker;
    std::atomic<bool> d_cancel{ false };

    // Worker observations.
    std::atomic<int> d_worker_state{ 0 }; // 0 idle, 1 trying, 2 connected, 3 exited
    std::atomic<int> d_attempts{ 0 };
    std::atomic<int> d_connections{ 0 };
    std::atomic<int> d_setup_errors{ 0 };
    std::atomic<std::int64_t> d_max_wait_slice_us{ 0 };

    // Scheduler-side observations.
    std::atomic<std::uint64_t> d_work_calls{ 0 };
    std::atomic<std::uint64_t> d_items_consumed{ 0 };
    std::atomic<std::uint64_t> d_work_calls_seeing_done{ 0 };

    mutable std::mutex d_mutex; // start/stop bookkeeping
    int d_start_calls = 0;
    int d_stop_calls = 0;
    int d_effective_stops = 0;
    bool d_running = false;
    bool d_stop_input_done = false;
    std::int64_t d_stop_items_pending = -1;
    double d_stop_ms = 0.0;
    double d_max_stop_ms = 0.0;

    void worker_main();
    void shutdown_worker();

public:
    m0_probe_sink_impl(bool listener, const std::string& host, int port);
    ~m0_probe_sink_impl() override;

    bool start() override;
    bool stop() override;
    std::string report() const override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_IMPL_H */
