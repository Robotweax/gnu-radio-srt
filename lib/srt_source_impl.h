/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_IMPL_H
#define INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_IMPL_H

#include "block_common.h"
#include "bounded_record_queue.h"
#include "session_worker.h"

#include <gnuradio/robotweax_srt/srt_source.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace gr {
namespace robotweax_srt {

class srt_source_impl : public srt_source
{
private:
    const transport_config d_public_transport;
    const stream_config d_public_stream;
    const rwsrt::stream_config d_stream;
    rwsrt::transport_config d_transport; // passphrase filled in at start()
    const bool d_strict;
    const std::size_t d_floats_per_item;

    std::unique_ptr<rwsrt::bounded_record_queue> d_queue;
    std::unique_ptr<rwsrt::session_worker> d_worker;

    // Scheduler-thread state: the record being emitted. Touched only by
    // work(), so it needs no lock.
    rwsrt::queue_entry d_current;
    bool d_have_current = false;
    std::uint32_t d_current_emitted = 0; // samples of d_current already output
    bool d_finished = false;             // WORK_DONE has been decided

    // Status thread: publishes periodically; never touches stream items.
    std::thread d_status_thread;
    std::mutex d_status_thread_mutex;
    std::condition_variable d_status_thread_wake;
    bool d_status_thread_stop = false;

    // start() could not prepare the run (for example no passphrase). The
    // block then finishes at once and the reason is in the status.
    std::atomic<bool> d_start_failed{ false };

    mutable std::mutex d_state_mutex; // everything below
    bool d_running = false;
    std::uint64_t d_samples_emitted = 0;
    std::uint64_t d_records_emitted = 0;
    std::uint64_t d_discontinuity_tags = 0;
    std::uint64_t d_capture_time_tags = 0;
    std::uint64_t d_untagged_discontinuities = 0; // breaks with no sample after them
    std::uint64_t d_reported_missing_samples = 0;
    std::uint64_t d_unknown_breaks = 0;
    std::uint64_t d_queue_drop_breaks = 0;
    bool d_end_emitted = false;
    bool d_have_position = false;
    rwsrt::coordinate d_position;        // next sample expected at the output
    double d_last_stop_ms = 0.0;
    rwsrt::session_snapshot d_final_session;
    rwsrt::queue_stats d_final_queue;

    void status_loop();
    void stop_status_thread();
    void note_break(const rwsrt::discontinuity& brk, bool tagged);
    rwsrt::status_doc status_doc() const;

public:
    srt_source_impl(const transport_config& transport, const stream_config& stream);
    ~srt_source_impl() override;

    bool start() override;
    bool stop() override;
    std::string status_json() const override;
    pmt::pmt_t status_pmt() const override;

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_IMPL_H */
