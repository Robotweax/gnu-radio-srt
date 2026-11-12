/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_CONFIG_H
#define INCLUDED_ROBOTWEAX_SRT_CONFIG_H

#include <gnuradio/robotweax_srt/api.h>
#include <cstdint>
#include <string>

namespace gr {
namespace robotweax_srt {

enum class sample_format { f32, cf32 };
enum class connection_role { caller, listener };
enum class continuity_policy { strict_stop, segmented_live };
enum class security_mode { none, ctr };

/*!
 * \brief How one block reaches its peer.
 * \ingroup robotweax_srt
 *
 * Connection role is independent of sample direction: a Sink may call or
 * listen. All values are immutable while the block runs and are validated
 * when the block is constructed.
 */
struct ROBOTWEAX_SRT_API transport_config {
    connection_role role = connection_role::caller;
    std::string host;                       //!< caller: numeric IPv4 of the peer
    std::string bind_address = "127.0.0.1"; //!< listener: change explicitly for remote access
    int port = 9000;
    std::string stream_id = "rwsrt/v1/default"; //!< routing label, not an identity
    int latency_ms = 250;                   //!< requested SRT receiver and peer latency
    int connect_timeout_ms = 3000;          //!< per attempt
    int startup_timeout_ms = 30000;         //!< no peer by then: terminal failure
    int peer_idle_timeout_ms = 5000;        //!< silence after which the connection counts as lost
    bool reconnect = true;                  //!< effective in segmented mode only
    int reconnect_initial_ms = 250;
    int reconnect_max_ms = 4000;            //!< delay cap, with +-20 % jitter
    int reconnect_total_timeout_ms = 60000; //!< budget after a connection failed
    int stop_timeout_ms = 1000;
    security_mode security = security_mode::ctr; //!< never downgraded automatically
    std::string passphrase_env;             //!< NAME of the environment variable, not the secret
    int status_interval_ms = 1000;
};

/*!
 * \brief What the block carries and how it buffers.
 * \ingroup robotweax_srt
 *
 * Format and rate must match on both ends; a mismatch is a terminal error on
 * the Source, never a silent conversion.
 */
struct ROBOTWEAX_SRT_API stream_config {
    sample_format format = sample_format::cf32;
    std::uint32_t sample_rate_num = 100000; //!< samples per second, numerator
    std::uint32_t sample_rate_den = 1;
    int record_limit_bytes = 1200;          //!< whole record, 96-byte header included
    int flush_interval_ms = 2;              //!< longest wait of a partial record
    std::uint64_t queue_max_bytes = 4u * 1024u * 1024u;
    int queue_max_records = 8192;
    int queue_max_age_ms = 100;
    continuity_policy continuity = continuity_policy::strict_stop;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_CONFIG_H */
