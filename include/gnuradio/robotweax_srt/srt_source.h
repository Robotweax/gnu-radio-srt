/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_H
#define INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_H

#include <gnuradio/robotweax_srt/api.h>
#include <gnuradio/robotweax_srt/config.h>
#include <gnuradio/sync_block.h>
#include <pmt/pmt.h>
#include <string>

namespace gr {
namespace robotweax_srt {

/*!
 * \brief Receives RWSR version 1 records from an SRT Sink.
 * \ingroup robotweax_srt
 *
 * One output stream of float (f32) or complex (cf32) items and a message
 * output port "status".
 *
 * Output tags:
 * - rwsrt.record on the first sample of every record: dict with run_id,
 *   segment_id, first_sample_index, sample_count, record_sequence,
 *   rate_num, rate_den.
 * - rwsrt.discontinuity on the first sample after a break: dict with
 *   reasons, previous and next coordinates, missing_known,
 *   missing_samples, connection_generation.
 * - rwsrt.capture_time on the first sample of a record that carries one:
 *   dict {time_ns, uncertainty_ns}.
 *
 * Samples on either side of a discontinuity tag are NOT continuous
 * acquisition. Downstream blocks that ignore tags must not be used after
 * this block in segmented_live mode. No sample is ever invented: a break
 * with no sample after it is published on "status" instead.
 *
 * The block finishes (WORK_DONE) after END, or on a terminal failure.
 * Which of the two happened is in status().
 */
class ROBOTWEAX_SRT_API srt_source : virtual public gr::sync_block
{
public:
    typedef std::shared_ptr<srt_source> sptr;

    //! Throws std::invalid_argument for any invalid configuration value.
    static sptr make(const transport_config& transport, const stream_config& stream);

    //! Current status as one JSON object. Contains no secret.
    virtual std::string status_json() const = 0;
    //! The same content as a PMT dictionary, as published on "status".
    virtual pmt::pmt_t status_pmt() const = 0;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_SRT_SOURCE_H */
