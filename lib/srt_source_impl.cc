/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "srt_source_impl.h"

#include <gnuradio/constants.h>
#include <gnuradio/io_signature.h>

#include <boost/thread/thread.hpp>

#include <algorithm>
#include <chrono>
#include <span>

namespace gr {
namespace robotweax_srt {

namespace {
using steady = std::chrono::steady_clock;
constexpr auto wait_slice = std::chrono::milliseconds(20);
} // namespace

srt_source::sptr srt_source::make(const transport_config& transport, const stream_config& stream)
{
    return gnuradio::make_block_sptr<srt_source_impl>(transport, stream);
}

srt_source_impl::srt_source_impl(const transport_config& transport, const stream_config& stream)
    : gr::sync_block("srt_source",
                     gr::io_signature::make(0, 0, 0),
                     gr::io_signature::make(1, 1, blk::item_size(stream.format))),
      d_public_transport(transport),
      d_public_stream(stream),
      // Invalid values are rejected here, before any worker or socket exists.
      d_stream(blk::to_internal(stream)),
      d_transport(blk::to_internal(transport)),
      d_strict(stream.continuity == continuity_policy::strict_stop),
      d_floats_per_item(stream.format == sample_format::f32 ? 1 : 2)
{
    message_port_register_out(blk::port_status);
    set_tag_propagation_policy(TPP_DONT);
}

srt_source_impl::~srt_source_impl()
{
    stop_status_thread();
    if (d_worker)
        d_worker->stop();
}

bool srt_source_impl::start()
{
    std::unique_lock<std::mutex> state(d_state_mutex);
    if (d_running)
        return sync_block::start();
    d_start_failed.store(false);
    try {
        blk::resolve_secret(d_public_transport, d_transport);
    } catch (const std::exception& e) {
        // start() runs on the scheduler thread: an exception here would kill
        // that thread and hang the flowgraph. Fail visibly instead.
        d_final_session = rwsrt::session_snapshot{};
        d_final_session.state = rwsrt::session_state::failed;
        d_final_session.failure = rwsrt::session_failure::configuration;
        d_final_session.failure_detail = e.what();
        d_final_queue = rwsrt::queue_stats{};
        d_start_failed.store(true);
        d_logger->error("{}: {}", alias(), e.what());
        return sync_block::start();
    }
    // Receiver side: discarded samples are reported on the next record.
    d_queue = std::make_unique<rwsrt::bounded_record_queue>(
        d_stream.queue,
        d_strict ? rwsrt::overflow_policy::fail : rwsrt::overflow_policy::drop_oldest,
        true);
    d_worker = std::make_unique<rwsrt::session_worker>(rwsrt::stream_direction::receive,
                                                       d_transport, d_stream, *d_queue);
    d_have_current = false;
    d_current_emitted = 0;
    d_finished = false;
    d_samples_emitted = d_records_emitted = 0;
    d_discontinuity_tags = d_capture_time_tags = d_untagged_discontinuities = 0;
    d_reported_missing_samples = d_unknown_breaks = d_queue_drop_breaks = 0;
    d_end_emitted = false;
    d_have_position = false;
    d_final_session = rwsrt::session_snapshot{};
    d_final_queue = rwsrt::queue_stats{};
    d_worker->start();
    d_running = true;
    state.unlock();

    {
        std::lock_guard<std::mutex> guard(d_status_thread_mutex);
        d_status_thread_stop = false;
    }
    d_status_thread = std::thread([this] { status_loop(); });
    return sync_block::start();
}

void srt_source_impl::stop_status_thread()
{
    {
        std::lock_guard<std::mutex> guard(d_status_thread_mutex);
        d_status_thread_stop = true;
    }
    d_status_thread_wake.notify_all();
    if (d_status_thread.joinable())
        d_status_thread.join();
}

bool srt_source_impl::stop()
{
    const auto t0 = steady::now();
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        if (!d_running)
            return sync_block::stop();
    }
    stop_status_thread();
    bool end_emitted;
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        end_emitted = d_end_emitted;
    }
    // After END the worker completes within one wait slice of being told;
    // let it, so the final status says "completed" rather than "draining".
    if (end_emitted)
        (void)d_worker->wait_finished(std::min<std::chrono::milliseconds>(
            d_transport.stop_timeout, std::chrono::milliseconds(200)));
    const rwsrt::session_snapshot session = d_worker->snapshot();
    const rwsrt::queue_stats queue = d_queue->stats();
    d_worker->stop(); // closes its sockets on its own thread, then joins
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        d_final_session = session;
        d_final_queue = queue;
        d_running = false;
        d_last_stop_ms = std::chrono::duration<double, std::milli>(steady::now() - t0).count();
    }
    message_port_pub(blk::port_status, status_pmt());
    return sync_block::stop();
}

void srt_source_impl::status_loop()
{
    std::unique_lock<std::mutex> lock(d_status_thread_mutex);
    while (!d_status_thread_stop) {
        d_status_thread_wake.wait_for(
            lock, std::chrono::milliseconds(d_public_transport.status_interval_ms));
        if (d_status_thread_stop)
            break;
        lock.unlock();
        message_port_pub(blk::port_status, status_pmt()); // no lock held
        lock.lock();
    }
}

void srt_source_impl::note_break(const rwsrt::discontinuity& brk, bool tagged)
{
    std::lock_guard<std::mutex> state(d_state_mutex);
    if (tagged)
        ++d_discontinuity_tags;
    else
        ++d_untagged_discontinuities;
    if (brk.missing_known)
        d_reported_missing_samples += brk.missing_samples;
    else
        ++d_unknown_breaks;
    if (brk.reasons & rwsrt::reason_receiver_queue_drop)
        ++d_queue_drop_breaks;
}

int srt_source_impl::work(int noutput_items,
                          gr_vector_const_void_star& /*input_items*/,
                          gr_vector_void_star& output_items)
{
    if (d_finished || d_start_failed.load())
        return WORK_DONE;
    float* out = static_cast<float*>(output_items[0]);
    int produced = 0;

    while (produced < noutput_items) {
        if (!d_have_current) {
            // Get the next record. With nothing produced yet this waits in
            // bounded slices inside work(): returning 0 would park the block
            // for about 250 ms on this scheduler (decisions D7). With items
            // already produced it only takes what is ready.
            rwsrt::queue_entry next;
            const rwsrt::pop_status st =
                produced > 0 ? d_queue->pop(next, steady::now())
                             : d_queue->wait_pop(next, wait_slice);
            if (st == rwsrt::pop_status::expired) {
                // Strict mode: a record that waited too long is terminal.
                d_worker->fail(rwsrt::session_failure::queue_overflow,
                               "queued record outlived queue_max_age");
                d_finished = true;
                break;
            }
            if (st != rwsrt::pop_status::record) {
                if (produced > 0)
                    break;
                const rwsrt::session_state s = d_worker->snapshot().state;
                if (s == rwsrt::session_state::failed || s == rwsrt::session_state::stopped) {
                    d_finished = true; // terminal failure: reported in status()
                    break;
                }
                if (boost::this_thread::interruption_requested())
                    return 0; // the scheduler loop honours the stop
                continue;
            }
            if (next.header.kind == rwsrt::record_kind::end) {
                // END is reported only after every queued sample before it
                // was emitted, which the queue order guarantees.
                if (next.brk) {
                    // A break with no sample after it: no sample is invented
                    // to carry a tag; it goes to the status port instead.
                    note_break(*next.brk, false);
                    pmt::pmt_t ev = pmt::make_dict();
                    ev = pmt::dict_add(ev, pmt::intern("event"), pmt::intern("discontinuity"));
                    ev = pmt::dict_add(ev, pmt::intern("at"), pmt::intern("end"));
                    ev = pmt::dict_add(ev, pmt::intern("discontinuity"),
                                       blk::discontinuity_pmt(*next.brk));
                    message_port_pub(blk::port_status, ev);
                }
                {
                    std::lock_guard<std::mutex> state(d_state_mutex);
                    d_end_emitted = true;
                    d_have_position = true;
                    d_position = rwsrt::coordinate{ next.header.run_id, next.header.segment_id,
                                                    next.header.first_sample_index };
                }
                d_worker->consumer_drained();
                d_finished = true;
                break;
            }
            d_current = std::move(next);
            d_current_emitted = 0;
            d_have_current = true;

            // Tags go on the first output sample of the record, once.
            const std::uint64_t offset = nitems_written(0) + static_cast<std::uint64_t>(produced);
            const rwsrt::record_header& h = d_current.header;
            pmt::pmt_t rec = pmt::make_dict();
            rec = pmt::dict_add(rec, pmt::intern("run_id"), pmt::intern(blk::run_id_hex(h.run_id)));
            rec = pmt::dict_add(rec, pmt::intern("segment_id"), pmt::from_uint64(h.segment_id));
            rec = pmt::dict_add(rec, pmt::intern("first_sample_index"),
                                pmt::from_uint64(h.first_sample_index));
            rec = pmt::dict_add(rec, pmt::intern("sample_count"), pmt::from_uint64(h.sample_count));
            rec = pmt::dict_add(rec, pmt::intern("record_sequence"),
                                pmt::from_uint64(h.record_sequence));
            rec = pmt::dict_add(rec, pmt::intern("rate_num"), pmt::from_uint64(h.rate_num));
            rec = pmt::dict_add(rec, pmt::intern("rate_den"), pmt::from_uint64(h.rate_den));
            add_item_tag(0, offset, blk::key_record, rec, alias_pmt());
            if (d_current.brk) {
                add_item_tag(0, offset, blk::key_discontinuity,
                             blk::discontinuity_pmt(*d_current.brk), alias_pmt());
                note_break(*d_current.brk, true);
            }
            if (h.capture_time_valid) {
                add_item_tag(0, offset, blk::key_capture_time,
                             blk::capture_time_pmt(h.capture_time_ns, h.capture_uncertainty_ns),
                             alias_pmt());
                std::lock_guard<std::mutex> state(d_state_mutex);
                ++d_capture_time_tags;
            }
        }

        // Emit as much of the current record as fits. A record split across
        // work() calls is completed before the next one starts.
        const std::uint32_t left = d_current.header.sample_count - d_current_emitted;
        const std::uint32_t take =
            std::min<std::uint32_t>(left, static_cast<std::uint32_t>(noutput_items - produced));
        const std::size_t bytes_per_sample = rwsrt::sample_size(d_current.header.format);
        rwsrt::unpack_f32le(
            std::span<const std::uint8_t>(d_current.message.data() + rwsrt::header_size +
                                              d_current_emitted * bytes_per_sample,
                                          take * bytes_per_sample),
            std::span<float>(out + static_cast<std::size_t>(produced) * d_floats_per_item,
                             take * d_floats_per_item));
        d_current_emitted += take;
        produced += static_cast<int>(take);
        const bool record_done = d_current_emitted == d_current.header.sample_count;
        {
            std::lock_guard<std::mutex> state(d_state_mutex);
            d_samples_emitted += take;
            d_have_position = true;
            d_position = rwsrt::coordinate{ d_current.header.run_id, d_current.header.segment_id,
                                            d_current.header.first_sample_index + d_current_emitted };
            if (record_done)
                ++d_records_emitted;
        }
        if (record_done)
            d_have_current = false;
    }
    if (produced > 0)
        return produced; // WORK_DONE, if decided, is returned by the next call
    if (d_finished) {
        // Best effort: subscribers inside this flowgraph may already be
        // shutting down. status() after the run is the reliable source.
        message_port_pub(blk::port_status, status_pmt());
        return WORK_DONE;
    }
    return 0;
}

rwsrt::status_doc srt_source_impl::status_doc() const
{
    rwsrt::status_doc d;
    rwsrt::session_snapshot session;
    rwsrt::queue_stats queue;
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        if (d_running) {
            session = d_worker->snapshot();
            queue = d_queue->stats();
        } else {
            session = d_final_session;
            queue = d_final_queue;
        }
        d.set_str("block", "srt_source")
            .set_str("block_id", alias())
            .set_str("direction", "receive")
            .set_bool("running", d_running)
            .set_uint("output_samples_emitted", d_samples_emitted)
            .set_uint("records_emitted", d_records_emitted)
            .set_uint("discontinuity_tags", d_discontinuity_tags)
            .set_uint("untagged_discontinuities", d_untagged_discontinuities)
            .set_uint("reported_missing_samples", d_reported_missing_samples)
            .set_uint("unknown_breaks", d_unknown_breaks)
            .set_uint("receiver_queue_drop_breaks", d_queue_drop_breaks)
            .set_uint("capture_time_tags", d_capture_time_tags)
            .set_bool("end_emitted", d_end_emitted)
            .set_num("last_stop_ms", d_last_stop_ms);
        if (d_have_position)
            d.set_str("run_id", blk::run_id_hex(d_position.run))
                .set_uint("segment_id", d_position.segment)
                .set_uint("next_sample_index", d_position.sample_index);
    }
    d.set_child("config", blk::describe(d_public_transport, d_public_stream));
    d.set_child("rx_queue", rwsrt::describe(queue));
    d.set_child("session", rwsrt::describe(session));
    d.child("versions")
        .set_str("module", RWSRT_MODULE_VERSION)
        .set_str("gnuradio", gr::version())
        .set_str("robotweax_srt", rwsrt::srt_library_version());
    // Leases this module holds on the process-wide SRT runtime: 0 when no
    // block of this module is running.
    d.set_int("srt_runtime_leases", rwsrt::runtime_owner::active_leases());
    return d;
}

std::string srt_source_impl::status_json() const { return status_doc().to_json(); }
pmt::pmt_t srt_source_impl::status_pmt() const { return blk::to_pmt(status_doc()); }

} /* namespace robotweax_srt */
} /* namespace gr */
