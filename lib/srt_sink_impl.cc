/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "srt_sink_impl.h"

#include <gnuradio/block_detail.h>
#include <gnuradio/buffer_reader.h>
#include <gnuradio/constants.h>
#include <gnuradio/io_signature.h>

#include <boost/thread/thread.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

namespace gr {
namespace robotweax_srt {

namespace {
using steady = std::chrono::steady_clock;
constexpr auto wait_slice = std::chrono::milliseconds(20);
constexpr auto housekeeping_period = std::chrono::milliseconds(1);

bool dict_uint(const pmt::pmt_t& dict, const char* key, std::uint64_t& out)
{
    const pmt::pmt_t v = pmt::dict_ref(dict, pmt::intern(key), pmt::PMT_NIL);
    if (pmt::is_uint64(v)) {
        out = pmt::to_uint64(v);
        return true;
    }
    if (pmt::is_integer(v) && pmt::to_long(v) >= 0) {
        out = static_cast<std::uint64_t>(pmt::to_long(v));
        return true;
    }
    return false;
}
} // namespace

srt_sink::sptr srt_sink::make(const transport_config& transport, const stream_config& stream)
{
    return gnuradio::make_block_sptr<srt_sink_impl>(transport, stream);
}

srt_sink_impl::srt_sink_impl(const transport_config& transport, const stream_config& stream)
    : gr::block("srt_sink",
                gr::io_signature::make(1, 1, blk::item_size(stream.format)),
                gr::io_signature::make(0, 0, 0)),
      d_public_transport(transport),
      d_public_stream(stream),
      // Invalid values are rejected here, before any worker or socket exists.
      d_stream(blk::to_internal(stream)),
      d_transport(blk::to_internal(transport)),
      d_strict(stream.continuity == continuity_policy::strict_stop),
      d_floats_per_item(stream.format == sample_format::f32 ? 1 : 2)
{
    message_port_register_out(blk::port_status);
    // Tags are interpreted here and never forwarded as they are.
    set_tag_propagation_policy(TPP_DONT);
}

srt_sink_impl::~srt_sink_impl()
{
    stop_housekeeper();
    if (d_worker)
        d_worker->stop();
}

bool srt_sink_impl::start()
{
    std::unique_lock<std::mutex> state(d_state_mutex);
    if (d_running)
        return block::start();
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
        return block::start();
    }

    // A block start is a new run: it never continues an earlier one.
    rwsrt::run_id_t run{};
    std::random_device rd;
    for (auto& b : run)
        b = static_cast<std::uint8_t>(rd());
    rwsrt::assembler_config ac;
    ac.format = d_stream.format;
    ac.rate_num = d_stream.rate_num;
    ac.rate_den = d_stream.rate_den;
    ac.record_limit_bytes = d_stream.record_limit_bytes;
    ac.flush_interval = d_stream.flush_interval;

    d_queue = std::make_unique<rwsrt::bounded_record_queue>(
        d_stream.queue,
        d_strict ? rwsrt::overflow_policy::fail : rwsrt::overflow_policy::drop_oldest,
        false);
    state.unlock();
    {
        std::lock_guard<std::mutex> assembly(d_assembly_mutex);
        d_assembler = std::make_unique<rwsrt::record_assembler>(ac, run);
    }
    state.lock();
    d_worker = std::make_unique<rwsrt::session_worker>(rwsrt::stream_direction::send, d_transport,
                                                       d_stream, *d_queue);
    d_run = run;
    d_segment = 0;
    d_next_index = 0;
    d_overflowed.store(false);
    d_natural_end = false;
    d_end_enqueued = false;
    d_samples_consumed = 0;
    d_records_assembled = 0;
    d_untransported_tags = d_malformed_tags = d_capture_time_tags = 0;
    d_upstream_known_gap_samples = d_upstream_known_gaps = d_upstream_unknown_gaps = 0;
    d_final_session = rwsrt::session_snapshot{};
    d_final_queue = rwsrt::queue_stats{};
    d_worker->start();
    d_running = true;
    state.unlock();

    {
        std::lock_guard<std::mutex> guard(d_housekeeper_mutex);
        d_housekeeper_stop = false;
    }
    d_housekeeper = std::thread([this] { housekeeping(); });
    return block::start();
}

void srt_sink_impl::stop_housekeeper()
{
    {
        std::lock_guard<std::mutex> guard(d_housekeeper_mutex);
        d_housekeeper_stop = true;
    }
    d_housekeeper_wake.notify_all();
    if (d_housekeeper.joinable())
        d_housekeeper.join();
}

bool srt_sink_impl::stop()
{
    const auto t0 = steady::now();
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        if (!d_running)
            return block::stop();
    }
    // Natural upstream completion, as opposed to cancellation: the input is
    // marked done and every item was consumed (docs/decisions.md D5).
    bool natural = false;
    if (const block_detail_sptr d = this->detail()) {
        if (d->ninputs() > 0 && d->input(0))
            natural = d->input(0)->done() && d->input(0)->items_available() == 0;
    }
    stop_housekeeper();

    const bool failed = session_failed();
    if (natural && !failed) {
        // Flush the partial record and send END, then give the worker a
        // bounded time to hand both to SRT. END is never sent on cancel.
        {
            std::lock_guard<std::mutex> assembly(d_assembly_mutex);
            (void)d_assembler->finish([this](rwsrt::queue_entry&& e) { push_record(std::move(e)); });
        }
        {
            std::lock_guard<std::mutex> state(d_state_mutex);
            d_end_enqueued = true;
        }
        // The worker bounds its own drain inside stop_timeout.
        (void)d_worker->wait_finished(d_transport.stop_timeout);
    }

    const rwsrt::session_snapshot session = d_worker->snapshot();
    const rwsrt::queue_stats queue = d_queue->stats();
    d_worker->stop(); // closes its sockets on its own thread, then joins
    std::uint64_t records, segment, next_index;
    {
        std::lock_guard<std::mutex> assembly(d_assembly_mutex);
        records = d_assembler->records_assembled();
        segment = d_assembler->segment();
        next_index = d_assembler->next_sample_index();
    }
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        d_natural_end = natural && !failed;
        d_final_session = session;
        d_final_queue = queue;
        d_records_assembled = records;
        d_segment = segment;
        d_next_index = next_index;
        d_running = false;
        d_last_stop_ms = std::chrono::duration<double, std::milli>(steady::now() - t0).count();
    }
    message_port_pub(blk::port_status, status_pmt());
    return block::stop();
}

void srt_sink_impl::fail(rwsrt::session_failure reason, const std::string& detail_text)
{
    if (d_worker)
        d_worker->fail(reason, detail_text);
}

bool srt_sink_impl::session_failed() const
{
    if (d_overflowed.load())
        return true;
    return d_worker && d_worker->snapshot().failure != rwsrt::session_failure::none;
}

// Called with the assembly lock held (lock order: assembly, then queue).
void srt_sink_impl::push_record(rwsrt::queue_entry&& entry)
{
    const rwsrt::push_status ps = d_queue->push(std::move(entry), steady::now());
    if (ps == rwsrt::push_status::ok)
        return;
    // Strict mode: a record that cannot be admitted is a terminal error,
    // never a silent drop. (Segmented mode drops oldest inside the queue.)
    d_overflowed.store(true);
    fail(rwsrt::session_failure::queue_overflow, "transmit queue full");
}

void srt_sink_impl::housekeeping()
{
    auto next_status = steady::now();
    std::unique_lock<std::mutex> lock(d_housekeeper_mutex);
    while (!d_housekeeper_stop) {
        d_housekeeper_wake.wait_for(lock, housekeeping_period);
        if (d_housekeeper_stop)
            break;
        lock.unlock();
        {
            // The flush deadline also holds when upstream delivers nothing.
            std::lock_guard<std::mutex> assembly(d_assembly_mutex);
            if (d_assembler)
                (void)d_assembler->flush_if_due(
                    steady::now(), [this](rwsrt::queue_entry&& e) { push_record(std::move(e)); });
        }
        if (steady::now() >= next_status) {
            next_status = steady::now() +
                          std::chrono::milliseconds(d_public_transport.status_interval_ms);
            message_port_pub(blk::port_status, status_pmt()); // no lock held
        }
        lock.lock();
    }
}

void srt_sink_impl::forecast(int /*noutput_items*/, gr_vector_int& ninput_items_required)
{
    ninput_items_required[0] = 1;
}

// Interprets one input tag. The caller has already framed every sample
// before the tag's position. Returns false on a terminal error.
bool srt_sink_impl::apply_tag(const gr::tag_t& tag)
{
    const auto emit = [this](rwsrt::queue_entry&& e) { push_record(std::move(e)); };
    if (pmt::eqv(tag.key, blk::key_capture_time)) {
        std::uint64_t time = 0, uncertainty = 0;
        const bool ok = pmt::is_dict(tag.value) && dict_uint(tag.value, "time_ns", time) &&
                        dict_uint(tag.value, "uncertainty_ns", uncertainty) &&
                        time <= static_cast<std::uint64_t>(INT64_MAX);
        rwsrt::assembler_status st = rwsrt::assembler_status::bad_time;
        if (ok) {
            std::lock_guard<std::mutex> assembly(d_assembly_mutex);
            st = d_assembler->set_capture_time(
                rwsrt::capture_time{ static_cast<std::int64_t>(time), uncertainty }, emit);
        }
        std::lock_guard<std::mutex> state(d_state_mutex);
        if (st == rwsrt::assembler_status::ok)
            ++d_capture_time_tags;
        else
            ++d_malformed_tags; // no time is invented from a tag that cannot be read
        return true;
    }
    if (pmt::eqv(tag.key, blk::key_input_discontinuity)) {
        std::uint64_t missing = 0;
        const bool known = pmt::is_dict(tag.value) && dict_uint(tag.value, "missing_samples", missing);
        rwsrt::assembler_status st;
        {
            std::lock_guard<std::mutex> assembly(d_assembly_mutex);
            // A discontinuity that cannot be read is still a discontinuity:
            // it is treated as an unknown gap, never ignored.
            st = known ? d_assembler->known_gap(missing, emit) : d_assembler->unknown_gap(emit);
        }
        {
            std::lock_guard<std::mutex> state(d_state_mutex);
            if (known) {
                ++d_upstream_known_gaps;
                d_upstream_known_gap_samples += missing;
            } else {
                ++d_upstream_unknown_gaps;
                const pmt::pmt_t u = pmt::is_dict(tag.value)
                                         ? pmt::dict_ref(tag.value, pmt::intern("unknown"), pmt::PMT_NIL)
                                         : pmt::PMT_NIL;
                if (!pmt::is_true(u) || pmt::is_null(u))
                    ++d_malformed_tags;
            }
        }
        if (st == rwsrt::assembler_status::exhausted) {
            fail(rwsrt::session_failure::internal, "sample coordinates exhausted; restart the block");
            return false;
        }
        if (d_strict) {
            // Strict mode: a loss of continuity upstream is as terminal as
            // one on the network. Nothing after it is sent as if continuous.
            fail(rwsrt::session_failure::continuity,
                 known ? "upstream sample gap in strict mode"
                       : "unknown upstream gap in strict mode");
            return false;
        }
        return true;
    }
    if (pmt::eqv(tag.key, blk::key_rx_rate) && pmt::is_number(tag.value)) {
        // A rate change inside a run is rejected, never silently ignored.
        const double configured = static_cast<double>(d_stream.rate_num) / d_stream.rate_den;
        const double tagged = pmt::to_double(tag.value);
        if (std::fabs(tagged - configured) > 1e-9 * configured) {
            fail(rwsrt::session_failure::configuration,
                 "rx_rate tag differs from the configured sample rate");
            return false;
        }
    }
    std::lock_guard<std::mutex> state(d_state_mutex);
    ++d_untransported_tags;
    return true;
}

int srt_sink_impl::general_work(int /*noutput_items*/,
                                gr_vector_int& ninput_items,
                                gr_vector_const_void_star& input_items,
                                gr_vector_void_star& /*output_items*/)
{
    if (d_start_failed.load() || session_failed())
        return WORK_DONE; // the failure is in status(); nothing is consumed

    if (d_strict) {
        // Strict mode accepts no sample before a peer is connected: the
        // queue would age out and fail the run at once (decisions D16).
        // Waiting happens here in bounded slices; returning 0 would park
        // the block for about 250 ms on this scheduler (decisions D7).
        for (;;) {
            const rwsrt::session_state s = d_worker->snapshot().state;
            if (s == rwsrt::session_state::connected)
                break;
            if (s == rwsrt::session_state::failed || s == rwsrt::session_state::stopped ||
                s == rwsrt::session_state::completed)
                return WORK_DONE;
            if (boost::this_thread::interruption_requested())
                return 0;
            (void)d_worker->wait_connected(wait_slice);
        }
    }

    const int n = ninput_items[0];
    const float* in = static_cast<const float*>(input_items[0]);
    const std::uint64_t base = nitems_read(0);
    std::vector<gr::tag_t> tags;
    get_tags_in_range(tags, 0, base, base + static_cast<std::uint64_t>(n));
    std::stable_sort(tags.begin(), tags.end(),
                     [](const gr::tag_t& a, const gr::tag_t& b) { return a.offset < b.offset; });

    const auto emit = [this](rwsrt::queue_entry&& e) { push_record(std::move(e)); };
    int cursor = 0;
    std::size_t tag_i = 0;
    while (cursor < n || tag_i < tags.size()) {
        // Frame the samples up to the next tag, then apply the tag at its
        // exact item position.
        const int limit = tag_i < tags.size() ? static_cast<int>(tags[tag_i].offset - base) : n;
        if (limit > cursor) {
            std::size_t consumed = 0;
            rwsrt::assembler_status st;
            {
                std::lock_guard<std::mutex> assembly(d_assembly_mutex);
                st = d_assembler->append(in + static_cast<std::size_t>(cursor) * d_floats_per_item,
                                         static_cast<std::size_t>(limit - cursor), steady::now(),
                                         emit, consumed);
            }
            cursor += static_cast<int>(consumed);
            if (st != rwsrt::assembler_status::ok) {
                fail(rwsrt::session_failure::internal,
                     "sample coordinates exhausted; restart the block");
                break;
            }
        }
        if (tag_i < tags.size()) {
            if (!apply_tag(tags[tag_i]))
                break;
            ++tag_i;
        }
        if (session_failed())
            break;
    }
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        d_samples_consumed += static_cast<std::uint64_t>(cursor);
    }
    // Consumption is exactly what was framed (or deliberately discarded by
    // the queue policy in segmented mode); nothing else is claimed.
    consume(0, cursor);
    return session_failed() ? WORK_DONE : 0;
}

rwsrt::status_doc srt_sink_impl::status_doc() const
{
    rwsrt::status_doc d;
    rwsrt::session_snapshot session;
    rwsrt::queue_stats queue;
    std::uint64_t records = 0, segment = 0, next_index = 0;
    rwsrt::run_id_t run{};
    bool partial = false, live = false;
    {
        std::lock_guard<std::mutex> assembly(d_assembly_mutex);
        if (d_assembler) {
            records = d_assembler->records_assembled();
            segment = d_assembler->segment();
            next_index = d_assembler->next_sample_index();
            partial = d_assembler->has_partial();
            live = true;
        }
    }
    {
        std::lock_guard<std::mutex> state(d_state_mutex);
        if (d_running) {
            session = d_worker->snapshot();
            queue = d_queue->stats();
        } else {
            session = d_final_session;
            queue = d_final_queue;
            partial = false;
            if (!live) {
                records = d_records_assembled;
                segment = d_segment;
                next_index = d_next_index;
            }
        }
        run = d_run;
        d.set_str("block", "srt_sink")
            .set_str("block_id", alias())
            .set_str("direction", "send")
            .set_bool("running", d_running)
            .set_str("run_id", blk::run_id_hex(run))
            .set_uint("segment_id", segment)
            .set_uint("next_sample_index", next_index)
            .set_uint("input_samples_consumed", d_samples_consumed)
            .set_uint("records_assembled", records)
            .set_bool("partial_record_pending", partial)
            .set_uint("capture_time_tags", d_capture_time_tags)
            .set_uint("upstream_known_gaps", d_upstream_known_gaps)
            .set_uint("upstream_known_gap_samples", d_upstream_known_gap_samples)
            .set_uint("upstream_unknown_gaps", d_upstream_unknown_gaps)
            .set_uint("untransported_tags", d_untransported_tags)
            .set_uint("malformed_tags", d_malformed_tags)
            .set_bool("natural_end", d_natural_end)
            .set_bool("end_enqueued", d_end_enqueued)
            .set_num("last_stop_ms", d_last_stop_ms);
    }
    // send_accepted means accepted into SRT's local buffer, not received.
    d.set_child("config", blk::describe(d_public_transport, d_public_stream));
    d.set_child("tx_queue", rwsrt::describe(queue));
    d.set_child("session", rwsrt::describe(session));
    d.child("versions")
        .set_str("gnuradio", gr::version())
        .set_str("robotweax_srt", rwsrt::srt_library_version());
    // Leases this module holds on the process-wide SRT runtime: 0 when no
    // block of this module is running.
    d.set_int("srt_runtime_leases", rwsrt::runtime_owner::active_leases());
    return d;
}

std::string srt_sink_impl::status_json() const { return status_doc().to_json(); }
pmt::pmt_t srt_sink_impl::status_pmt() const { return blk::to_pmt(status_doc()); }

} /* namespace robotweax_srt */
} /* namespace gr */
