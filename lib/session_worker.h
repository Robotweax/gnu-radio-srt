/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_SESSION_WORKER_H
#define INCLUDED_RWSRT_SESSION_WORKER_H

// Network worker: owns the sockets of one session and moves complete records
// between a bounded queue and SRT (plan section 9). Shared by the GNU Radio
// blocks and the headless reference peers.

#include "bounded_record_queue.h"
#include "continuity.h"
#include "runtime_owner.h"
#include "session_config.h"
#include "session_state.h"
#include "srt_adapter.h"
#include "status_doc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rwsrt {

enum class end_observation {
    none,               //!< no END seen (yet)
    complete,           //!< receiver-observed complete run
    incomplete          //!< END seen, but samples of the run were missed
};

//! Counters are named by measurement layer. send_accepted means accepted
//! into SRT's local buffer; it never means received by the peer.
struct session_counters {
    std::uint64_t connect_attempts = 0;
    std::uint64_t connections = 0;
    std::uint64_t connection_losses = 0;
    std::uint64_t rejected_peers = 0;
    std::uint64_t records_send_accepted = 0;
    std::uint64_t samples_send_accepted = 0;
    std::uint64_t bytes_send_accepted = 0;
    std::uint64_t send_would_block = 0;
    std::uint64_t stale_records_dropped = 0; //!< held across an outage, too old to send
    std::uint64_t stale_samples_dropped = 0;
    std::uint64_t records_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t malformed_records = 0;
    std::uint64_t late_records_dropped = 0; //!< receiver: later than max_lateness
    std::uint64_t late_samples_dropped = 0;
    std::int64_t max_lateness_us = 0;       //!< receiver: largest lateness seen on this connection set
    std::uint64_t end_sent = 0;
    std::uint64_t end_repeats_sent = 0;
    std::uint64_t peer_notices_sent = 0;     //!< receiver: told the sender why it ended
    std::uint64_t peer_notices_received = 0; //!< sender //!< extra transmissions of the END message
    std::uint64_t max_wait_slice_us = 0;
};

struct session_snapshot {
    session_state state = session_state::stopped;
    session_failure failure = session_failure::none;
    std::string failure_detail; //!< never contains the passphrase
    int error_code = 0;
    std::uint64_t connection_generation = 0;
    session_counters counters;
    ledger_counters continuity; //!< receive direction
    end_observation end = end_observation::none;
    bool termination_unknown = false; //!< run was open when the session ended
    bool completed = false;           //!< reached COMPLETED; kept after stop()
    //! Sender: after END, SRT reported nothing left unacknowledged, so the
    //! peer's SRT stack received every packet. Not an application
    //! acknowledgement, and false does not prove a loss.
    bool end_acknowledged_by_transport = false;
    srt_effective effective;
    srt_link_stats link;
};

//! Structured description of a snapshot. No secrets.
status_doc describe(const session_snapshot& s);
status_doc describe(const queue_stats& q);
//! One JSON object, for JSON Lines output.
std::string to_json(const session_snapshot& s);
//! Robotweax SRT release this module was built against, e.g. "0.3.2".
std::string srt_library_version();

class session_worker
{
public:
    /*!
     * \p queue outlives the worker. In the send direction the worker pops
     * records from it; in the receive direction it pushes validated records.
     * Throws std::invalid_argument when a configuration does not validate.
     */
    session_worker(stream_direction direction,
                   const transport_config& transport,
                   const stream_config& stream,
                   bounded_record_queue& queue);
    ~session_worker();
    session_worker(const session_worker&) = delete;
    session_worker& operator=(const session_worker&) = delete;

    void start();
    //! Cancels, lets the worker close its own sockets, joins. Idempotent.
    void stop();

    //! Reports a terminal condition found outside the worker, for example a
    //! strict-mode queue overflow seen by the producer.
    void fail(session_failure reason, const std::string& detail);
    //! Receive direction: the consumer has taken END out of the queue.
    void consumer_drained();

    session_snapshot snapshot() const;
    //! Blocks until the session is connected, finished or \p timeout passed.
    bool wait_connected(std::chrono::milliseconds timeout) const;
    //! Blocks until completed, failed or stopped, or \p timeout passed.
    bool wait_finished(std::chrono::milliseconds timeout) const;

private:
    using clock = std::chrono::steady_clock;

    void run();
    bool establish(srt_endpoint& ep, retry_schedule& retry, bool reconnecting);
    void serve_send(srt_endpoint& ep);
    void serve_receive(srt_endpoint& ep, continuity_ledger& ledger);
    void repeat_end(srt_endpoint& ep, const queue_entry& end);
    void notify_peer(srt_endpoint& ep);
    bool read_peer_notice(srt_endpoint& ep);
    void drain_send(srt_endpoint& ep);
    bool cancelled() const noexcept { return d_cancel.load(std::memory_order_relaxed); }
    bool apply_external_failure();
    void fatal(session_failure reason, const std::string& detail, int code = 0);
    void publish(srt_endpoint* ep = nullptr);
    void note_slice(clock::time_point started);
    void pause(std::chrono::nanoseconds duration);

    const stream_direction d_direction;
    const transport_config d_transport;
    const stream_config d_stream;
    bounded_record_queue& d_queue;

    std::shared_ptr<runtime_owner::lease> d_runtime;
    std::thread d_thread;
    std::atomic<bool> d_cancel{ false };
    std::atomic<bool> d_consumer_drained{ false };

    // Worker-thread state. The state machine is touched only under d_mutex.
    mutable std::mutex d_mutex;
    mutable std::condition_variable d_changed;
    session_fsm d_fsm;
    session_snapshot d_status;
    session_failure d_external_failure = session_failure::none;
    std::string d_external_detail;
    bool d_started = false;
    //! END taken from the queue but not yet accepted by SRT. Unlike DATA it
    //! is kept across a reconnect: it never becomes stale.
    std::optional<queue_entry> d_held_end;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_SESSION_WORKER_H */
