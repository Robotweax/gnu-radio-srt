/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_SRT_SINK_H
#define INCLUDED_ROBOTWEAX_SRT_SRT_SINK_H

#include <gnuradio/block.h>
#include <gnuradio/robotweax_srt/api.h>
#include <gnuradio/robotweax_srt/config.h>
#include <pmt/pmt.h>
#include <string>

namespace gr {
namespace robotweax_srt {

/*!
 * \brief Sends a sample stream to an SRT Source as RWSR version 1 records.
 * \ingroup robotweax_srt
 *
 * One input stream of float (f32) or complex (cf32) items and a message
 * output port "status".
 *
 * Input tags, at the exact item they apply to:
 * - rwsrt.capture_time: dict {time_ns, uncertainty_ns}, Unix nanoseconds of
 *   that sample.
 * - rwsrt.input_discontinuity: dict {missing_samples: n} for an exact gap
 *   before that sample, or {unknown: true}.
 * Other tags are not transported; they are counted.
 *
 * In strict_stop mode nothing is consumed until a peer is connected, and
 * any loss of continuity ends the block with a terminal failure in the
 * status. In segmented_live mode old records are discarded instead and every
 * discarded sample stays visible in sample coordinates and counters.
 *
 * Natural upstream completion flushes the partial record and sends END.
 * A flowgraph stop is cancellation and sends no END.
 *
 * Whether the block ended in success or failure is in status(), never in
 * the mere fact that the flowgraph finished.
 */
class ROBOTWEAX_SRT_API srt_sink : virtual public gr::block
{
public:
    typedef std::shared_ptr<srt_sink> sptr;

    //! Throws std::invalid_argument for any invalid configuration value.
    static sptr make(const transport_config& transport, const stream_config& stream);

    //! Current status as one JSON object. Contains no secret.
    virtual std::string status_json() const = 0;
    //! The same content as a PMT dictionary, as published on "status".
    virtual pmt::pmt_t status_pmt() const = 0;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_SRT_SINK_H */
