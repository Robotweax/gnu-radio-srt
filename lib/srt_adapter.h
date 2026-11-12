/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_SRT_ADAPTER_H
#define INCLUDED_RWSRT_SRT_ADAPTER_H

// The only code that talks to SRT: public C API of <srt/srt.h>, translated
// into internal values (plan section 8). No SRT type appears in this header.

#include "session_config.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace rwsrt {

//! Option values read back from the library, -1 where a read failed.
struct srt_effective {
    int rcv_latency_ms = -1;
    int peer_latency_ms = -1;
    int payload_size = -1;
    int mss = -1;
    int snd_buf_bytes = -1;
    int rcv_buf_bytes = -1;
    int flow_window_packets = -1;
    int udp_snd_buf_bytes = -1;
    int udp_rcv_buf_bytes = -1;
    std::int64_t max_bw = -1;
    std::int64_t input_bw = -1;
    int overhead_bw_percent = -1;
    int key_length = -1;
    int km_state = -1; //!< 0 unsecured, 2 secured (SRT_KM_STATE)
    std::string peer_stream_id;
};

struct srt_link_stats {
    bool valid = false;
    double rtt_ms = 0.0;
    std::int64_t packets_sent = 0;
    std::int64_t packets_received = 0;
    int send_loss = 0;
    int receive_loss = 0;
    int retransmitted = 0;
    int send_dropped = 0;
    int receive_dropped = 0;
    int undecrypted = 0;
    int send_buffer_packets = 0;
    int receive_buffer_packets = 0;
};

enum class connect_progress { pending, connected, failed };
enum class connect_failure { none, transient, credentials, configuration };
enum class io_result { ok, would_block, lost, error };

enum wait_bits : unsigned { wait_timeout = 0, wait_readable = 1, wait_writable = 2, wait_broken = 4 };

/*!
 * One Caller or Listener endpoint with at most one active connection.
 * Not thread-safe: exactly one worker thread owns it, and that thread is the
 * only one that ever closes its sockets. Every call returns within its
 * slice; nothing blocks indefinitely. The SRT runtime must be held by a
 * runtime_owner lease for the whole lifetime of the object.
 */
class srt_endpoint
{
public:
    //! \p offered_bw is the sender's byte rate (0 on a receiver).
    srt_endpoint(const transport_config& config, std::size_t record_limit_bytes,
                 std::int64_t offered_bw);
    ~srt_endpoint();
    srt_endpoint(const srt_endpoint&) = delete;
    srt_endpoint& operator=(const srt_endpoint&) = delete;

    //! Caller: creates a socket and starts a nonblocking connect. Listener:
    //! binds and listens once; later calls only re-arm it. False on a local
    //! failure, described by error().
    bool open();

    //! Advances connection establishment, waiting at most \p slice_ms.
    connect_progress poll(int slice_ms);
    connect_failure failure() const noexcept { return d_failure; }
    bool connected() const noexcept;

    //! Waits at most \p slice_ms for the connection. Returns wait_bits.
    //! Extra peers arriving at a listener are rejected here.
    unsigned wait(int slice_ms, bool want_read, bool want_write);

    io_result send(const std::uint8_t* data, std::size_t size);
    //! One whole message into \p buffer (capacity must cover the record limit).
    io_result receive(std::uint8_t* buffer, std::size_t capacity, std::size_t& size);
    //! Packets handed to SRT that the peer has not acknowledged; -1 unknown.
    int unacknowledged_packets();

    void close_connection(); //!< a listener keeps listening
    void close();            //!< everything

    srt_effective effective();
    srt_link_stats stats();
    std::uint64_t rejected_peers() const noexcept { return d_rejected; }
    const std::string& error() const noexcept { return d_error; }
    int error_code() const noexcept { return d_error_code; }

private:
    bool configure(int socket);
    void set_error(const char* what);
    void reject(int socket);

    const transport_config d_config;
    const std::size_t d_record_limit;
    const std::int64_t d_offered_bw;

    int d_listener;
    int d_connection;
    int d_pending; //!< caller socket whose connect is in progress
    int d_epoll;
    connect_failure d_failure = connect_failure::none;
    std::uint64_t d_rejected = 0;
    std::string d_error;
    int d_error_code = 0;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_SRT_ADAPTER_H */
