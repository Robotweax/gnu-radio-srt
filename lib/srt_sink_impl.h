/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_SRT_SINK_IMPL_H
#define INCLUDED_ROBOTWEAX_SRT_SRT_SINK_IMPL_H

#include "block_common.h"
#include "bounded_record_queue.h"
#include "record_assembler.h"
#include "session_worker.h"

#include <gnuradio/robotweax_srt/srt_sink.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace gr {
namespace robotweax_srt {

class srt_sink_impl : public srt_sink
{
private:
    const transport_config d_public_transport;
    const stream_config d_public_stream;
    const rwsrt::stream_config d_stream;
    rwsrt::transport_config d_transport; // passphrase filled in at start()
    const bool d_strict;
    const std::size_t d_floats_per_item;

    // Run objects, created by start() and released by stop().
    std::unique_ptr<rwsrt::bounded_record_queue> d_queue;
    std::unique_ptr<rwsrt::session_worker> d_worker;

    // Assembly lock: guards the assembler. Lock order: assembly, then the
    // transmit queue (inside emit). No socket call is made under either.
    mutable std::mutex d_assembly_mutex;
    std::unique_ptr<rwsrt::record_assembler> d_assembler;

    // Housekeeping thread: flushes an expired partial record when upstream
    // pauses and publishes status. It never touches stream items or tags.
    std::thread d_housekeeper;
    std::mutex d_housekeeper_mutex;
    std::condition_variable d_housekeeper_wake;
    bool d_housekeeper_stop = false;

    // Set under the assembly lock, read anywhere: atomic, so the assembly
    // and state locks are never held together.
    std::atomic<bool> d_overflowed{ false };

    // start() could not prepare the run (for example no passphrase). The
    // block then finishes at once and the reason is in the status.
    std::atomic<bool> d_start_failed{ false };

    mutable std::mutex d_state_mutex; // everything below
    bool d_running = false;
    bool d_natural_end = false;       // last stop followed upstream completion
    bool d_end_enqueued = false;
    std::uint64_t d_samples_consumed = 0;
    std::uint64_t d_records_assembled = 0;
    std::uint64_t d_untransported_tags = 0;
    std::uint64_t d_malformed_tags = 0;
    std::uint64_t d_capture_time_tags = 0;
    std::uint64_t d_upstream_known_gap_samples = 0;
    std::uint64_t d_upstream_known_gaps = 0;
    std::uint64_t d_upstream_unknown_gaps = 0;
    double d_last_stop_ms = 0.0;
    rwsrt::session_snapshot d_final_session; // kept after stop()
    rwsrt::queue_stats d_final_queue;
    rwsrt::run_id_t d_run{};
    std::uint64_t d_segment = 0;
    std::uint64_t d_next_index = 0;

    void push_record(rwsrt::queue_entry&& entry);
    bool apply_tag(const gr::tag_t& tag);
    void housekeeping();
    void stop_housekeeper();
    void fail(rwsrt::session_failure reason, const std::string& detail);
    bool session_failed() const;
    rwsrt::status_doc status_doc() const;

public:
    srt_sink_impl(const transport_config& transport, const stream_config& stream);
    ~srt_sink_impl() override;

    bool start() override;
    bool stop() override;
    std::string status_json() const override;
    pmt::pmt_t status_pmt() const override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;
    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_SRT_SINK_IMPL_H */
