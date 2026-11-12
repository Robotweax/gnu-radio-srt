/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_BLOCK_COMMON_H
#define INCLUDED_ROBOTWEAX_SRT_BLOCK_COMMON_H

// Shared by srt_sink_impl and srt_source_impl: conversion from the public
// configuration to the validated internal one, tag keys, PMT rendering.

#include "continuity.h"
#include "session_config.h"
#include "status_doc.h"

#include <gnuradio/robotweax_srt/config.h>
#include <pmt/pmt.h>

#include <cstddef>
#include <string>

namespace gr {
namespace robotweax_srt {
namespace blk {

extern const pmt::pmt_t key_capture_time;        // rwsrt.capture_time
extern const pmt::pmt_t key_input_discontinuity; // rwsrt.input_discontinuity
extern const pmt::pmt_t key_record;              // rwsrt.record
extern const pmt::pmt_t key_discontinuity;       // rwsrt.discontinuity
extern const pmt::pmt_t key_rx_rate;             // rx_rate (rate changes are rejected)
extern const pmt::pmt_t port_status;             // status

std::size_t item_size(sample_format f);

//! Converts and validates everything that does not need the secret.
//! Throws std::invalid_argument naming the first problem.
rwsrt::stream_config to_internal(const stream_config& s);
rwsrt::transport_config to_internal(const transport_config& t);

//! Reads the passphrase from the configured environment variable and
//! validates the result. Throws std::runtime_error; the message never
//! contains the secret.
void resolve_secret(const transport_config& pub, rwsrt::transport_config& internal);

//! Public configuration as a status subtree. The secret is not in it.
rwsrt::status_doc describe(const transport_config& t, const stream_config& s);

pmt::pmt_t to_pmt(const rwsrt::status_doc& doc);
std::string run_id_hex(const rwsrt::run_id_t& run);
pmt::pmt_t coordinate_pmt(const rwsrt::coordinate& c);
pmt::pmt_t discontinuity_pmt(const rwsrt::discontinuity& d);
pmt::pmt_t capture_time_pmt(std::int64_t time_ns, std::uint64_t uncertainty_ns);

} // namespace blk
} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_BLOCK_COMMON_H */
