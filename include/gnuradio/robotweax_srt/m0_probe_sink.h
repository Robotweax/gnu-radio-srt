/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_H
#define INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_H

#include <gnuradio/block.h>
#include <gnuradio/robotweax_srt/api.h>
#include <string>

namespace gr {
namespace robotweax_srt {

/*!
 * \brief M0 scaffolding: lifecycle probe for the future SRT Sink.
 * \ingroup robotweax_srt
 *
 * Consumes and discards complex samples while a worker thread drives a
 * nonblocking SRT Caller or Listener that never needs a peer. It exists to
 * qualify, on the pinned GNU Radio runtime, how natural upstream completion
 * differs from user cancellation and that stop() is bounded with no peer.
 * It carries no samples and is replaced by srt_sink in milestone M2.
 */
class ROBOTWEAX_SRT_API m0_probe_sink : virtual public gr::block
{
public:
    typedef std::shared_ptr<m0_probe_sink> sptr;

    /*!
     * \param listener true: bind and listen on host:port; false: call it.
     * \param host numeric IPv4 address.
     * \param port UDP port, 1-65535.
     */
    static sptr make(bool listener, const std::string& host, int port);

    //! Observations so far as one JSON object.
    virtual std::string report() const = 0;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SINK_H */
