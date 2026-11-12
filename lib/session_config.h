/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_SESSION_CONFIG_H
#define INCLUDED_RWSRT_SESSION_CONFIG_H

// Validated, immutable configuration shared with workers (plan section 5).
// No SRT types: this header is usable by pure-core code and tests.

#include "bounded_record_queue.h"
#include "continuity.h"
#include "session_state.h"
#include "wire_codec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rwsrt {

enum class security_mode { none, ctr };
enum class stream_direction { send, receive };

struct stream_config {
    sample_format format = sample_format::cf32;
    std::uint32_t rate_num = 100000;
    std::uint32_t rate_den = 1;
    std::size_t record_limit_bytes = 1200;
    std::chrono::milliseconds flush_interval{ 2 };
    queue_limits queue;
    continuity_policy policy = continuity_policy::strict_stop;
};

struct transport_config {
    connection_role role = connection_role::caller;
    std::string host;                         //!< caller: numeric IPv4 of the peer
    std::string bind_address = "127.0.0.1";   //!< listener: local numeric IPv4
    int port = 9000;
    std::string stream_id = "rwsrt/v1/default";
    int latency_ms = 250;
    int connect_timeout_ms = 3000;
    int peer_idle_timeout_ms = 5000;
    std::chrono::milliseconds startup_timeout{ 30000 };
    bool reconnect = true;                    //!< effective in segmented mode only
    retry_config retry;
    std::chrono::milliseconds stop_timeout{ 1000 };
    security_mode security = security_mode::ctr;
    std::string passphrase;                   //!< resolved by the caller; never logged
    int key_length = 16;                      //!< AES key bytes: 16, 24 or 32
    int srt_buffer_bytes = 8 * 1024 * 1024;   //!< SRTO_SNDBUF and SRTO_RCVBUF
};

//! Largest record the qualified transport carries in one packet
//! (IPv4, MSS 1500; docs/decisions.md D9).
inline constexpr std::size_t qualified_payload_ceiling = 1456;

//! Empty when the configuration is usable, otherwise the first problem.
//! Never includes the passphrase.
std::string validate(const stream_config& s);
std::string validate(const transport_config& t);

//! Upper bound of the application byte rate handed to SRT, framing
//! included: payload plus one header per full record and per flush interval.
std::int64_t offered_bytes_per_second(const stream_config& s);

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_SESSION_CONFIG_H */
