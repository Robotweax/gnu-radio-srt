/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_worker.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rwsrt {

namespace {

constexpr int wait_slice_ms = 20;          // plan section 9; see decisions D6
constexpr auto stats_interval = std::chrono::milliseconds(200);
constexpr int receive_batch = 64;          // messages per slice before re-checking cancel

std::string json_escape(const std::string& in)
{
    std::string out;
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

std::string to_json(const session_snapshot& s)
{
    const session_counters& c = s.counters;
    std::ostringstream os;
    os << "{\"state\":\"" << to_string(s.state) << "\",\"failure\":\"" << to_string(s.failure)
       << "\",\"failure_detail\":\"" << json_escape(s.failure_detail)
       << "\",\"error_code\":" << s.error_code
       << ",\"connection_generation\":" << s.connection_generation
       << ",\"end\":\""
       << (s.end == end_observation::complete
               ? "complete"
               : s.end == end_observation::incomplete ? "incomplete" : "none")
       << "\",\"termination_unknown\":" << (s.termination_unknown ? "true" : "false")
       << ",\"completed\":" << (s.completed ? "true" : "false")
       << ",\"counters\":{\"connect_attempts\":" << c.connect_attempts
       << ",\"connections\":" << c.connections
       << ",\"connection_losses\":" << c.connection_losses
       << ",\"rejected_peers\":" << c.rejected_peers
       << ",\"records_send_accepted\":" << c.records_send_accepted
       << ",\"samples_send_accepted\":" << c.samples_send_accepted
       << ",\"bytes_send_accepted\":" << c.bytes_send_accepted
       << ",\"send_would_block\":" << c.send_would_block
       << ",\"stale_records_dropped\":" << c.stale_records_dropped
       << ",\"stale_samples_dropped\":" << c.stale_samples_dropped
       << ",\"records_received\":" << c.records_received
       << ",\"bytes_received\":" << c.bytes_received
       << ",\"malformed_records\":" << c.malformed_records
       << ",\"end_sent\":" << c.end_sent
       << ",\"max_wait_slice_ms\":" << static_cast<double>(c.max_wait_slice_us) / 1000.0
       << "},\"continuity\":{\"records_accepted\":" << s.continuity.records_accepted
       << ",\"samples_accepted\":" << s.continuity.samples_accepted
       << ",\"known_missing_samples\":" << s.continuity.known_missing_samples
       << ",\"unknown_gaps\":" << s.continuity.unknown_gaps
       << ",\"sequence_gaps\":" << s.continuity.sequence_gaps
       << ",\"discontinuities\":" << s.continuity.discontinuities
       << ",\"runs\":" << s.continuity.runs
       << "},\"effective\":{\"rcv_latency_ms\":" << s.effective.rcv_latency_ms
       << ",\"peer_latency_ms\":" << s.effective.peer_latency_ms
       << ",\"payload_size\":" << s.effective.payload_size
       << ",\"mss\":" << s.effective.mss
       << ",\"snd_buf_bytes\":" << s.effective.snd_buf_bytes
       << ",\"rcv_buf_bytes\":" << s.effective.rcv_buf_bytes
       << ",\"flow_window_packets\":" << s.effective.flow_window_packets
       << ",\"udp_snd_buf_bytes\":" << s.effective.udp_snd_buf_bytes
       << ",\"udp_rcv_buf_bytes\":" << s.effective.udp_rcv_buf_bytes
       << ",\"max_bw\":" << s.effective.max_bw
       << ",\"input_bw\":" << s.effective.input_bw
       << ",\"overhead_bw_percent\":" << s.effective.overhead_bw_percent
       << ",\"key_length\":" << s.effective.key_length
       << ",\"km_state\":" << s.effective.km_state
       << ",\"peer_stream_id\":\"" << json_escape(s.effective.peer_stream_id)
       << "\"},\"link\":{\"valid\":" << (s.link.valid ? "true" : "false")
       << ",\"rtt_ms\":" << s.link.rtt_ms
       << ",\"packets_sent\":" << s.link.packets_sent
       << ",\"packets_received\":" << s.link.packets_received
       << ",\"send_loss\":" << s.link.send_loss
       << ",\"receive_loss\":" << s.link.receive_loss
       << ",\"retransmitted\":" << s.link.retransmitted
       << ",\"send_dropped\":" << s.link.send_dropped
       << ",\"receive_dropped\":" << s.link.receive_dropped
       << ",\"undecrypted\":" << s.link.undecrypted << "}}";
    return os.str();
}

session_worker::session_worker(stream_direction direction,
                               const transport_config& transport,
                               const stream_config& stream,
                               bounded_record_queue& queue)
    : d_direction(direction),
      d_transport(transport),
      d_stream([&stream] {
          stream_config s = stream;
          normalize_rate(s.rate_num, s.rate_den);
          return s;
      }()),
      d_queue(queue),
      d_fsm(transport.role, stream.policy, transport.reconnect)
{
    // Reject invalid values before any worker or socket exists.
    if (const std::string e = validate(stream); !e.empty())
        throw std::invalid_argument("stream configuration: " + e);
    if (const std::string e = validate(transport); !e.empty())
        throw std::invalid_argument("transport configuration: " + e);
}

session_worker::~session_worker() { stop(); }

void session_worker::start()
{
    std::lock_guard<std::mutex> guard(d_mutex);
    if (d_started)
        return;
    d_runtime = runtime_owner::acquire();
    d_cancel.store(false);
    d_consumer_drained.store(false);
    d_held_end.reset();
    d_external_failure = session_failure::none;
    d_status = session_snapshot{};
    d_fsm.start();
    d_status.state = d_fsm.state();
    d_started = true;
    d_thread = std::thread([this] { run(); });
}

void session_worker::stop()
{
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        if (!d_started)
            return;
        d_started = false;
    }
    d_cancel.store(true);
    if (d_thread.joinable())
        d_thread.join(); // the worker closed its own sockets before exiting
    d_runtime.reset();   // only after every SRT handle of this session is closed
}

void session_worker::fail(session_failure reason, const std::string& detail)
{
    std::lock_guard<std::mutex> guard(d_mutex);
    if (d_external_failure == session_failure::none) {
        d_external_failure = reason;
        d_external_detail = detail;
    }
}

void session_worker::consumer_drained() { d_consumer_drained.store(true); }

session_snapshot session_worker::snapshot() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return d_status;
}

bool session_worker::wait_connected(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(d_mutex);
    d_changed.wait_for(lock, timeout, [this] {
        const session_state s = d_status.state;
        return s == session_state::connected || s == session_state::draining ||
               s == session_state::completed || s == session_state::failed ||
               s == session_state::stopped;
    });
    return d_status.state == session_state::connected;
}

bool session_worker::wait_finished(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(d_mutex);
    return d_changed.wait_for(lock, timeout, [this] {
        const session_state s = d_status.state;
        return s == session_state::completed || s == session_state::failed ||
               s == session_state::stopped;
    });
}

void session_worker::publish(srt_endpoint* ep)
{
    // Callers hold d_mutex.
    d_status.state = d_fsm.state();
    d_status.failure = d_fsm.failure();
    d_status.connection_generation = d_fsm.connection_generation();
    if (d_fsm.state() == session_state::completed)
        d_status.completed = true;
    if (ep)
        d_status.counters.rejected_peers = ep->rejected_peers();
    d_changed.notify_all();
}

void session_worker::fatal(session_failure reason, const std::string& detail, int code)
{
    std::lock_guard<std::mutex> guard(d_mutex);
    if (d_fsm.on_fatal(reason)) {
        d_status.failure_detail = detail;
        d_status.error_code = code;
    }
    publish();
}

bool session_worker::apply_external_failure()
{
    std::lock_guard<std::mutex> guard(d_mutex);
    if (d_external_failure == session_failure::none)
        return false;
    if (d_fsm.on_fatal(d_external_failure))
        d_status.failure_detail = d_external_detail;
    d_external_failure = session_failure::none;
    publish();
    return true;
}

void session_worker::note_slice(clock::time_point started)
{
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - started).count());
    std::lock_guard<std::mutex> guard(d_mutex);
    d_status.counters.max_wait_slice_us = std::max(d_status.counters.max_wait_slice_us, us);
}

void session_worker::pause(std::chrono::nanoseconds duration)
{
    const auto until = clock::now() + duration;
    while (!cancelled() && clock::now() < until) {
        const auto left = until - clock::now();
        std::this_thread::sleep_for(
            std::min<std::chrono::nanoseconds>(left, std::chrono::milliseconds(wait_slice_ms)));
    }
}

// Connects or accepts. Returns true when connected; false when the session
// is cancelled or has failed.
bool session_worker::establish(srt_endpoint& ep, retry_schedule& retry, bool reconnecting)
{
    const auto started = clock::now();
    for (;;) {
        if (cancelled() || apply_external_failure())
            return false;
        {
            std::lock_guard<std::mutex> guard(d_mutex);
            if (d_fsm.state() == session_state::retry_wait)
                d_fsm.on_retry_elapsed();
            ++d_status.counters.connect_attempts;
            publish(&ep);
        }
        if (!ep.open()) {
            fatal(session_failure::configuration, ep.error(), ep.error_code());
            return false;
        }
        connect_progress progress = connect_progress::pending;
        while (progress == connect_progress::pending) {
            if (cancelled() || apply_external_failure())
                return false;
            const auto now = clock::now();
            // Records must not age in the queue while nobody can send them.
            if (d_direction == stream_direction::send &&
                d_stream.policy == continuity_policy::segmented_live)
                (void)d_queue.expire(now);
            const bool deadline = reconnecting
                                      ? retry.budget_exhausted(now)
                                      : now - started >= d_transport.startup_timeout;
            if (deadline) {
                std::lock_guard<std::mutex> guard(d_mutex);
                if (reconnecting)
                    d_fsm.on_reconnect_budget_exhausted();
                else
                    d_fsm.on_startup_deadline();
                d_status.failure_detail = reconnecting ? "no peer within the reconnect budget"
                                                       : "no peer within the startup timeout";
                publish(&ep);
                return false;
            }
            const auto slice = clock::now();
            progress = ep.poll(wait_slice_ms);
            note_slice(slice);
        }
        if (progress == connect_progress::connected) {
            std::lock_guard<std::mutex> guard(d_mutex);
            d_fsm.on_connected();
            ++d_status.counters.connections;
            d_status.effective = ep.effective();
            publish(&ep);
            return true;
        }
        // Attempt failed.
        const connect_failure why = ep.failure();
        if (why == connect_failure::credentials || why == connect_failure::configuration) {
            std::lock_guard<std::mutex> guard(d_mutex);
            d_fsm.on_attempt_failed(attempt_failure::terminal,
                                    why == connect_failure::credentials
                                        ? session_failure::credentials
                                        : session_failure::configuration);
            d_status.failure_detail = ep.error();
            d_status.error_code = ep.error_code();
            publish(&ep);
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(d_mutex);
            d_fsm.on_attempt_failed(attempt_failure::transient, session_failure::none);
            publish(&ep);
        }
        // During startup the retry delay is bounded by the startup timeout
        // itself; during an outage by the reconnect budget.
        const auto delay = retry.next_delay(clock::now());
        if (!delay) {
            if (reconnecting) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_fsm.on_reconnect_budget_exhausted();
                d_status.failure_detail = "no peer within the reconnect budget";
                publish(&ep);
                return false;
            }
            pause(std::chrono::milliseconds(wait_slice_ms)); // let the deadline check decide
        } else {
            pause(*delay);
        }
    }
}

// After END was handed to SRT: give the library a bounded time to deliver
// what it holds. Closing at once would discard it (linger is off).
void session_worker::drain_send(srt_endpoint& ep)
{
    const auto limit =
        clock::now() + std::chrono::milliseconds(d_transport.latency_ms) + d_transport.stop_timeout;
    while (!cancelled() && clock::now() < limit) {
        const int pending = ep.unacknowledged_packets();
        if (pending <= 0)
            break;
        if (ep.wait(wait_slice_ms, false, false) & wait_broken)
            break;
    }
    std::lock_guard<std::mutex> guard(d_mutex);
    d_status.link = ep.stats();
    d_fsm.on_drained();
    publish(&ep);
}

void session_worker::serve_send(srt_endpoint& ep)
{
    queue_entry pending;
    bool have_pending = false;
    if (d_held_end) {
        pending = std::move(*d_held_end);
        d_held_end.reset();
        have_pending = true;
    }
    auto last_stats = clock::now();
    auto last_break_check = clock::now();
    for (;;) {
        if (cancelled() || apply_external_failure())
            return;
        if (!have_pending) {
            const pop_status st = d_queue.wait_pop(pending, std::chrono::milliseconds(wait_slice_ms));
            if (st == pop_status::expired) {
                fatal(session_failure::queue_overflow, "queued record outlived queue_max_age");
                return;
            }
            have_pending = st == pop_status::record;
        }
        // Notice a broken connection even while there is nothing to send.
        // While records flow, a failed send reports the break, so the
        // explicit check runs at most once per slice interval.
        if (!have_pending || clock::now() - last_break_check >= std::chrono::milliseconds(wait_slice_ms)) {
            last_break_check = clock::now();
            if (ep.wait(0, false, false) & wait_broken)
                break;
        }
        if (have_pending) {
            const io_result r = ep.send(pending.message.data(), pending.message.size());
            if (r == io_result::ok) {
                const bool is_end = pending.header.kind == record_kind::end;
                {
                    std::lock_guard<std::mutex> guard(d_mutex);
                    ++d_status.counters.records_send_accepted;
                    d_status.counters.samples_send_accepted += pending.header.sample_count;
                    d_status.counters.bytes_send_accepted += pending.message.size();
                    if (is_end) {
                        ++d_status.counters.end_sent;
                        d_fsm.on_end_of_stream();
                        publish(&ep);
                    }
                }
                have_pending = false;
                if (is_end) {
                    drain_send(ep);
                    return;
                }
            } else if (r == io_result::would_block) {
                {
                    std::lock_guard<std::mutex> guard(d_mutex);
                    ++d_status.counters.send_would_block;
                }
                const auto s2 = clock::now();
                const unsigned w = ep.wait(wait_slice_ms, false, true);
                note_slice(s2);
                if (w & wait_broken)
                    break;
            } else if (r == io_result::lost) {
                break;
            } else {
                fatal(session_failure::transport, ep.error(), ep.error_code());
                return;
            }
        }
        if (clock::now() - last_stats >= stats_interval) {
            last_stats = clock::now();
            std::lock_guard<std::mutex> guard(d_mutex);
            d_status.link = ep.stats();
            publish(&ep);
        }
    }
    // Connection lost. A record taken out of the queue but not accepted by
    // SRT is not silently lost: it is accounted here. It is not re-queued,
    // because after an outage it would no longer be current.
    std::lock_guard<std::mutex> guard(d_mutex);
    if (have_pending && pending.header.kind == record_kind::data) {
        ++d_status.counters.stale_records_dropped;
        d_status.counters.stale_samples_dropped += pending.header.sample_count;
    } else if (have_pending) {
        d_held_end = std::move(pending);
    }
    ++d_status.counters.connection_losses;
    d_status.link = ep.stats();
    d_fsm.on_connection_lost();
    if (d_fsm.state() == session_state::failed)
        d_status.failure_detail = "connection lost";
    publish(&ep);
}

void session_worker::serve_receive(srt_endpoint& ep, continuity_ledger& ledger)
{
    std::vector<std::uint8_t> buffer(std::max<std::size_t>(d_stream.record_limit_bytes, 1500));
    auto last_stats = clock::now();
    bool end_pushed = false;
    for (;;) {
        if (cancelled() || apply_external_failure())
            return;
        if (end_pushed) {
            // The stream ended. Nothing more is read; the session completes
            // when the consumer has taken END out of the queue.
            if (d_consumer_drained.load()) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_status.link = ep.stats();
                d_fsm.on_drained();
                publish(&ep);
                return;
            }
            pause(std::chrono::milliseconds(wait_slice_ms));
            continue;
        }
        const auto slice = clock::now();
        const unsigned bits = ep.wait(wait_slice_ms, true, false);
        note_slice(slice);
        bool lost = false;
        // Buffered messages are read before a break is acted on.
        for (int i = 0; i < receive_batch; ++i) {
            std::size_t size = 0;
            const io_result r = ep.receive(buffer.data(), buffer.size(), size);
            if (r == io_result::would_block)
                break;
            if (r == io_result::lost) {
                lost = true;
                break;
            }
            if (r == io_result::error) {
                fatal(session_failure::transport, ep.error(), ep.error_code());
                return;
            }
            decoded_record rec;
            const wire_error we = decode(std::span<const std::uint8_t>(buffer.data(), size),
                                         d_stream.record_limit_bytes, rec);
            if (we != wire_error::none) {
                {
                    std::lock_guard<std::mutex> guard(d_mutex);
                    ++d_status.counters.malformed_records;
                }
                fatal(session_failure::protocol,
                      std::string("malformed record: ") + to_string(we));
                return;
            }
            const ledger_result lr = ledger.on_record(rec.header);
            {
                std::lock_guard<std::mutex> guard(d_mutex);
                ++d_status.counters.records_received;
                d_status.counters.bytes_received += size;
                d_status.continuity = ledger.counters();
            }
            if (!lr.accepted) {
                const session_failure f =
                    lr.failure == ledger_failure::config_mismatch
                        ? session_failure::configuration
                        : lr.failure == ledger_failure::protocol_violation
                              ? session_failure::protocol
                              : session_failure::continuity;
                fatal(f, lr.detail);
                return;
            }
            queue_entry e;
            e.header = rec.header;
            e.message.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            e.brk = lr.brk;
            const push_status ps = d_queue.push(std::move(e), clock::now());
            if (ps == push_status::overflow || ps == push_status::too_large) {
                fatal(session_failure::queue_overflow, "receive queue full");
                return;
            }
            if (ps == push_status::after_end) {
                // A new run arrived while the previous END is still unread.
                fatal(session_failure::protocol, "record received after an unconsumed END");
                return;
            }
            if (lr.is_end) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_status.end = lr.end_complete ? end_observation::complete
                                               : end_observation::incomplete;
                d_fsm.on_end_of_stream();
                publish(&ep);
                end_pushed = true;
                break;
            }
        }
        if (end_pushed)
            continue;
        if (lost || (bits & wait_broken))
            break;
        if (clock::now() - last_stats >= stats_interval) {
            last_stats = clock::now();
            std::lock_guard<std::mutex> guard(d_mutex);
            d_status.link = ep.stats();
            publish(&ep);
        }
    }
    std::lock_guard<std::mutex> guard(d_mutex);
    ++d_status.counters.connection_losses;
    d_status.link = ep.stats();
    d_status.termination_unknown = ledger.run_open();
    d_fsm.on_connection_lost();
    if (d_fsm.state() == session_state::failed)
        d_status.failure_detail = ledger.run_open() ? "connection lost without END"
                                                    : "connection lost";
    publish(&ep);
}

void session_worker::run()
{
    {
        srt_endpoint ep(d_transport, d_stream.record_limit_bytes,
                        d_direction == stream_direction::send
                            ? offered_bytes_per_second(d_stream)
                            : 0);
        continuity_ledger ledger(d_stream.policy, d_stream.format, d_stream.rate_num,
                                 d_stream.rate_den);
        const auto seed = static_cast<std::uint64_t>(clock::now().time_since_epoch().count());
        // Startup attempts are bounded by the startup timeout, outages by the
        // reconnect budget.
        retry_config startup_retry = d_transport.retry;
        startup_retry.total_budget = d_transport.startup_timeout;
        retry_schedule startup(startup_retry, seed);
        retry_schedule outage(d_transport.retry, seed ^ 0x5bd1e995u);
        startup.begin(clock::now());

        bool reconnecting = false;
        while (!cancelled()) {
            if (!establish(ep, reconnecting ? outage : startup, reconnecting))
                break;
            if (d_direction == stream_direction::receive) {
                ledger.on_connection();
                {
                    std::lock_guard<std::mutex> guard(d_mutex);
                    d_status.termination_unknown = false;
                }
                serve_receive(ep, ledger);
            } else {
                serve_send(ep);
            }
            ep.close_connection();
            session_state state;
            {
                std::lock_guard<std::mutex> guard(d_mutex);
                state = d_fsm.state();
            }
            if (state != session_state::retry_wait)
                break; // completed, failed or cancelled
            reconnecting = true;
            outage.begin(clock::now());
        }
        // The session is over without END: say so rather than imply success.
        {
            std::lock_guard<std::mutex> guard(d_mutex);
            if (d_direction == stream_direction::receive && ledger.run_open())
                d_status.termination_unknown = true;
            d_status.continuity = ledger.counters();
            publish(&ep);
        }
        // Completed or failed sessions keep their state for the status
        // report until stop(); the sockets are released right away.
        ep.close();
    }
    while (!cancelled())
        pause(std::chrono::milliseconds(wait_slice_ms));
    std::lock_guard<std::mutex> guard(d_mutex);
    const session_failure failure = d_fsm.failure();
    d_fsm.request_stop();
    d_fsm.on_stopped();
    publish();
    d_status.failure = failure;
}

} // namespace rwsrt
