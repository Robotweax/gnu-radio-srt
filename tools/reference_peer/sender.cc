// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless reference sender: deterministic test signal -> RWSR records ->
// bounded queue -> SRT. Uses the same codec, queue and worker as the GNU
// Radio Sink, without GNU Radio, so transport problems can be told apart
// from scheduler problems. Prints one JSON object on stdout when it ends.
//
// Exit status: 0 completed (END handed to SRT), 2 session failed,
// 3 cancelled by --cancel-after-ms (no END), 64 usage error.

#include "peer_common.h"
#include "record_assembler.h"
#include "session_worker.h"

#include <chrono>
#include <cinttypes>
#include <random>
#include <thread>
#include <vector>

using namespace rwsrt;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    transport_config transport;
    stream_config stream;
    long long total_samples = 0, cancel_after_ms = 0, connect_wait_ms = 0;
    bool unpaced = false;
    try {
        rwsrt_peer::options o(argc, argv);
        rwsrt_peer::read_common(o, transport, stream);
        total_samples = o.num("samples", -1);      // -1: endless
        cancel_after_ms = o.num("cancel-after-ms", 0);
        unpaced = o.flag("unpaced");
        o.finish();
        if (total_samples < 0 && cancel_after_ms <= 0)
            throw std::invalid_argument("an endless run needs --cancel-after-ms");
        connect_wait_ms = transport.startup_timeout.count() + 1000;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sender: %s\n", e.what());
        return 64;
    }

    const bool strict = stream.policy == continuity_policy::strict_stop;
    bounded_record_queue queue(stream.queue,
                               strict ? overflow_policy::fail : overflow_policy::drop_oldest,
                               false);
    run_id_t run{};
    {
        std::random_device rd;
        for (auto& b : run)
            b = static_cast<std::uint8_t>(rd());
    }
    assembler_config ac;
    ac.format = stream.format;
    ac.rate_num = stream.rate_num;
    ac.rate_den = stream.rate_den;
    ac.record_limit_bytes = stream.record_limit_bytes;
    ac.flush_interval = stream.flush_interval;

    std::unique_ptr<session_worker> worker;
    std::unique_ptr<record_assembler> assembler;
    try {
        // The worker validates the whole configuration and names the problem.
        worker = std::make_unique<session_worker>(stream_direction::send, transport, stream, queue);
        assembler = std::make_unique<record_assembler>(ac, run);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sender: %s\n", e.what());
        return 64;
    }
    worker->start();

    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    bool overflowed = false, cancelled = false;
    const auto emit = [&](queue_entry&& e) {
        if (overflowed)
            return;
        const push_status ps = queue.push(std::move(e), clock::now());
        if (ps != push_status::ok) {
            // Strict mode: a full queue is a terminal error, never a silent drop.
            overflowed = true;
            worker->fail(session_failure::queue_overflow, "transmit queue full");
        }
    };

    // Strict mode produces nothing until a peer is connected; the queue would
    // otherwise overflow before the first record could be sent.
    bool proceed = true;
    if (strict)
        proceed = worker->wait_connected(std::chrono::milliseconds(connect_wait_ms));

    const std::size_t floats = sample_size(stream.format) / 4;
    const double rate = static_cast<double>(stream.rate_num) / stream.rate_den;
    std::vector<float> chunk;
    std::uint64_t produced = 0;
    const auto production_start = clock::now();
    while (proceed && !overflowed) {
        if (cancel_after_ms > 0 && clock::now() - started >= std::chrono::milliseconds(cancel_after_ms)) {
            cancelled = true;
            break;
        }
        if (total_samples >= 0 && produced >= static_cast<std::uint64_t>(total_samples))
            break;
        if (worker->snapshot().state == session_state::failed)
            break;
        // Paced like a sample clock: produce what is due by now, in 1 ms steps.
        std::uint64_t due;
        if (unpaced) {
            due = produced + 4096;
        } else {
            const double elapsed = std::chrono::duration<double>(clock::now() - production_start).count();
            due = static_cast<std::uint64_t>(elapsed * rate);
        }
        if (total_samples >= 0)
            due = std::min<std::uint64_t>(due, static_cast<std::uint64_t>(total_samples));
        if (due > produced) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(due - produced, 65536));
            chunk.resize(n * floats);
            for (std::size_t i = 0; i < n; ++i)
                rwsrt_peer::test_signal(stream.format, produced + i, &chunk[i * floats]);
            std::size_t consumed = 0;
            if (assembler->append(chunk.data(), n, clock::now(), emit, consumed) != assembler_status::ok) {
                worker->fail(session_failure::internal, "sample coordinates exhausted");
                break;
            }
            produced += consumed;
        }
        assembler->flush_if_due(clock::now(), emit);
        if (!unpaced)
            std::this_thread::sleep_for(1ms);
        else if (queue.stats().records > stream.queue.max_records / 2)
            std::this_thread::sleep_for(1ms); // unpaced still must not overrun its own queue
    }

    bool finished = false;
    if (proceed && !overflowed && !cancelled && worker->snapshot().state != session_state::failed) {
        // Natural end: flush the partial record and send END.
        (void)assembler->finish(emit);
        finished = worker->wait_finished(std::chrono::milliseconds(
            transport.latency_ms + transport.stop_timeout.count() + 5000 +
            (strict ? 0 : transport.retry.total_budget.count())));
    }
    const session_snapshot snap = worker->snapshot();
    const queue_stats qs = queue.stats();
    const auto stop_started = clock::now();
    worker->stop();
    const double stop_ms = std::chrono::duration<double, std::milli>(clock::now() - stop_started).count();

    std::printf("{\"peer\":\"sender\",\"samples_produced\":%" PRIu64 ",\"records_assembled\":%" PRIu64
                ",\"cancelled\":%s,\"queue_overflow\":%s,\"waited_for_end\":%s,"
                "\"offered_bytes_per_second\":%lld,\"stop_ms\":%.3f,\"runtime_leases_after_stop\":%d,"
                "\"library_log_messages\":%llu,\"queue\":%s,\"session\":%s}\n",
                produced, assembler->records_assembled(), cancelled ? "true" : "false",
                overflowed ? "true" : "false", finished ? "true" : "false",
                static_cast<long long>(offered_bytes_per_second(stream)), stop_ms,
                runtime_owner::active_leases(),
                static_cast<unsigned long long>(runtime_owner::library_log().messages),
                rwsrt_peer::queue_json(qs).c_str(), to_json(snap).c_str());
    if (snap.completed)
        return 0;
    if (snap.failure != session_failure::none || overflowed)
        return 2;
    return 3;
}
