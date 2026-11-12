// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless reference receiver: SRT -> validated RWSR records -> bounded
// queue -> sample verification against the deterministic test signal.
// Prints one JSON line per discontinuity on stderr (when --events is given)
// and one JSON object on stdout when it ends.
//
// Exit status: 0 the session completed (END observed) and no sample was
// altered, 2 session failed, 3 samples altered, 4 timed out, 64 usage error.
// Whether the run was complete is in the JSON ("end"), not the exit status.

#include "peer_common.h"
#include "peer_link.h"

#include <chrono>
#include <cinttypes>
#include <cstring>
#include <vector>

using namespace rwsrt;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    transport_config transport;
    stream_config stream;
    long long timeout_ms = 0, stall_after_samples = -1, stall_ms = 0;
    bool events = false;
    std::string timing_log;
    rwsrt_peer::link_kind kind = rwsrt_peer::link_kind::srt;
    try {
        rwsrt_peer::options o(argc, argv);
        rwsrt_peer::read_common(o, transport, stream);
        timeout_ms = o.num("timeout-ms", 60000);
        events = o.flag("events");
        timing_log = o.str("timing-log", "");
        kind = rwsrt_peer::parse_transport(o.str("transport", "srt"));
        stall_after_samples = o.num("stall-after-samples", -1); // simulate a slow consumer
        stall_ms = o.num("stall-ms", 0);
        o.finish();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "receiver: %s\n", e.what());
        return 64;
    }

    const bool strict = stream.policy == continuity_policy::strict_stop;
    bounded_record_queue queue(stream.queue,
                               strict ? overflow_policy::fail : overflow_policy::drop_oldest,
                               true);
    std::unique_ptr<rwsrt_peer::peer_link> worker;
    try {
        worker = std::make_unique<rwsrt_peer::peer_link>(kind, stream_direction::receive, transport, stream, queue);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "receiver: %s\n", e.what());
        return 64;
    }
    // Optional binary log, one entry per output record: first sample index
    // (u64), sample count (u32), reasons of a preceding break (u32), output
    // time on the monotonic clock in ns (i64). Little-endian, 24 bytes.
    std::FILE* timing = timing_log.empty() ? nullptr : std::fopen(timing_log.c_str(), "wb");
    worker->start();

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    const std::size_t floats = sample_size(stream.format) / 4;
    std::vector<float> samples;
    std::uint64_t received = 0, altered = 0, records = 0, breaks = 0;
    std::uint64_t missing_known = 0, unknown_breaks = 0, queue_drop_breaks = 0;
    std::uint64_t first_index = 0, last_end_index = 0;
    std::uint8_t reasons_seen = 0;
    bool have_first = false, end_seen = false, timed_out = false, stalled = false;

    while (!end_seen) {
        if (clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        queue_entry e;
        const pop_status st = queue.wait_pop(e, 20ms);
        if (st == pop_status::expired) {
            // Strict mode: a record that waited too long is a terminal error.
            worker->fail_overflow("queued record outlived queue_max_age");
            break;
        }
        if (st != pop_status::record) {
            if (worker->failed())
                break;
            continue;
        }
        if (e.brk) {
            ++breaks;
            reasons_seen |= e.brk->reasons;
            if (e.brk->missing_known)
                missing_known += e.brk->missing_samples;
            else
                ++unknown_breaks;
            if (e.brk->reasons & reason_receiver_queue_drop)
                ++queue_drop_breaks;
            if (events)
                std::fprintf(stderr,
                             "{\"event\":\"discontinuity\",\"reasons\":\"%s\",\"missing_known\":%s,"
                             "\"missing_samples\":%" PRIu64 ",\"next_segment\":%" PRIu64
                             ",\"next_index\":%" PRIu64 ",\"generation\":%" PRIu64 "}\n",
                             reasons_to_string(e.brk->reasons).c_str(),
                             e.brk->missing_known ? "true" : "false", e.brk->missing_samples,
                             e.brk->next.segment, e.brk->next.sample_index,
                             e.brk->connection_generation);
        }
        if (e.header.kind == record_kind::end) {
            end_seen = true;
            last_end_index = e.header.first_sample_index;
            worker->consumer_drained();
            break;
        }
        ++records;
        if (!have_first) {
            have_first = true;
            first_index = e.header.first_sample_index;
        }
        // Verify every sample against the value its coordinate implies.
        samples.resize(e.header.sample_count * floats);
        unpack_f32le(std::span<const std::uint8_t>(e.message.data() + header_size,
                                                   e.message.size() - header_size),
                     samples);
        float expect[2] = { 0.0f, 0.0f };
        for (std::uint32_t i = 0; i < e.header.sample_count; ++i) {
            rwsrt_peer::test_signal(stream.format, e.header.first_sample_index + i, expect);
            if (std::memcmp(expect, &samples[i * floats], floats * 4) != 0)
                ++altered;
        }
        received += e.header.sample_count;
        if (timing) {
            const std::uint64_t index = e.header.first_sample_index;
            const std::uint32_t count = e.header.sample_count;
            const std::uint32_t reasons = e.brk ? e.brk->reasons : 0u;
            const std::int64_t t = rwsrt_peer::monotonic_ns();
            std::fwrite(&index, 8, 1, timing);
            std::fwrite(&count, 4, 1, timing);
            std::fwrite(&reasons, 4, 1, timing);
            std::fwrite(&t, 8, 1, timing);
        }
        if (!stalled && stall_after_samples >= 0 &&
            received >= static_cast<std::uint64_t>(stall_after_samples)) {
            stalled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        }
    }
    if (end_seen)
        (void)worker->wait_finished(2000ms);
    else if (!timed_out)
        (void)worker->wait_finished(200ms);
    if (timing)
        std::fclose(timing);
    const std::string status = worker->status_json();
    const bool completed = worker->completed();
    const queue_stats qs = queue.stats();
    const auto stop_started = clock::now();
    worker->stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(clock::now() - stop_started).count();

    std::printf("{\"peer\":\"receiver\",\"samples_received\":%" PRIu64 ",\"samples_altered\":%" PRIu64
                ",\"records\":%" PRIu64 ",\"first_index\":%" PRIu64 ",\"end_seen\":%s,\"end_index\":%" PRIu64
                ",\"discontinuities\":%" PRIu64 ",\"reasons\":\"%s\",\"missing_known_samples\":%" PRIu64
                ",\"unknown_breaks\":%" PRIu64 ",\"queue_drop_breaks\":%" PRIu64
                ",\"timed_out\":%s,\"stop_ms\":%.3f,\"runtime_leases_after_stop\":%d,"
                "\"library_log_messages\":%llu,\"resources\":%s,\"queue\":%s,\"session\":%s}\n",
                received, altered, records, first_index, end_seen ? "true" : "false", last_end_index,
                breaks, reasons_to_string(reasons_seen).c_str(), missing_known, unknown_breaks,
                queue_drop_breaks, timed_out ? "true" : "false", stop_ms,
                runtime_owner::active_leases(),
                static_cast<unsigned long long>(runtime_owner::library_log().messages),
                rwsrt_peer::resource_json().c_str(), rwsrt_peer::queue_json(qs).c_str(),
                status.c_str());
    if (altered != 0)
        return 3;
    if (timed_out)
        return 4;
    if (completed)
        return 0;
    return 2;
}
