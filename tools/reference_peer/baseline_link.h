// SPDX-License-Identifier: GPL-3.0-or-later
//
// Baseline transports for the reference peers: plain UDP with a fixed
// playout delay, and TCP. They carry the same RWSR records through the same
// bounded queue and the same continuity ledger as the SRT session worker, so
// that a comparison measures the transport and nothing else.
//
//   udp  One record per datagram. No retransmission. The receiver holds each
//        record until its playout time (first arrival + latency + nominal
//        sample offset), releases records in order, and drops what arrives
//        after a later record was released. This gives UDP the same latency
//        budget that SRT gets; without it UDP could not even reorder.
//   tcp  One connection, records back to back, TCP_NODELAY, and a send
//        buffer of one latency budget of data so that stale samples are
//        discarded by the sender's queue and not parked in the kernel.
//        Records are released as they arrive.
//
// The sender connects (tcp) or sends (udp) to host:port; the receiver binds.
// Neither reconnects: an outage is whatever the kernel makes of it.

#ifndef RWSRT_BASELINE_LINK_H
#define RWSRT_BASELINE_LINK_H

#include "bounded_record_queue.h"
#include "continuity.h"
#include "sample_time.h"
#include "session_config.h"
#include "status_doc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace rwsrt_peer {

enum class link_kind { srt, udp, tcp };

struct baseline_counters {
    std::uint64_t records_sent = 0;
    std::uint64_t samples_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t send_errors = 0;       //!< datagram refused, or TCP write failed
    std::uint64_t connect_attempts = 0;
    std::uint64_t records_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t malformed_records = 0;
    std::uint64_t too_late_dropped = 0;  //!< udp: arrived after a later record was released
    std::uint64_t duplicates_dropped = 0;
    std::uint64_t playout_high_water = 0; //!< udp: most records held at once
    bool end_sent = false;
    bool connected = false;
};

class baseline_link
{
public:
    baseline_link(link_kind kind, rwsrt::stream_direction direction,
                  const rwsrt::transport_config& transport, const rwsrt::stream_config& stream,
                  rwsrt::bounded_record_queue& queue);
    ~baseline_link();

    void start();
    void stop();
    void fail(const std::string& reason);
    void consumer_drained() { d_consumer_drained.store(true); }

    bool connected() const;
    bool failed() const;
    bool completed() const;   //!< sender: END written; receiver: END consumed
    bool finished() const { return failed() || completed(); }
    rwsrt::status_doc describe() const; //!< shaped like the SRT session status

private:
    void run_send();
    void run_receive_udp();
    void run_receive_tcp();
    bool accept_record(const std::uint8_t* data, std::size_t size);
    void set_failure(const std::string& failure, const std::string& detail);

    const link_kind d_kind;
    const rwsrt::stream_direction d_direction;
    const rwsrt::transport_config d_transport;
    const rwsrt::stream_config d_stream;
    rwsrt::bounded_record_queue& d_queue;

    std::thread d_thread;
    std::atomic<bool> d_cancel{ false };
    std::atomic<bool> d_consumer_drained{ false };

    mutable std::mutex d_mutex;
    baseline_counters d_counters;
    rwsrt::continuity_ledger d_ledger;
    std::string d_failure = "none";
    std::string d_failure_detail;
    bool d_completed = false;
    bool d_end_received = false;
    bool d_end_complete = false;
};

} // namespace rwsrt_peer

#endif
