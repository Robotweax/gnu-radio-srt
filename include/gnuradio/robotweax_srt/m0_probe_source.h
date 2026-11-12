/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_H
#define INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_H

#include <gnuradio/robotweax_srt/api.h>
#include <gnuradio/sync_block.h>
#include <string>

namespace gr {
namespace robotweax_srt {

/*!
 * \brief M0 scaffolding: lifecycle probe for the future SRT Source.
 * \ingroup robotweax_srt
 *
 * A source fed only by inject(). It qualifies, on the pinned scheduler, two
 * ways of handling an empty queue: returning zero items after one bounded
 * wait, or waiting inside work() in bounded slices until data, completion or
 * cancellation. It also checks that stop() is bounded while waiting and that
 * WORK_DONE ends the flowgraph. It is replaced by srt_source in milestone M2.
 */
class ROBOTWEAX_SRT_API m0_probe_source : virtual public gr::sync_block
{
public:
    typedef std::shared_ptr<m0_probe_source> sptr;

    /*!
     * \param finish_after_ms return WORK_DONE once this long after start;
     *        0 or less: never finish on its own.
     * \param wait_inside_work false: wait once (20 ms) and return 0 items;
     *        true: keep waiting inside work() in 20 ms slices, checking for
     *        scheduler-thread interruption between slices.
     */
    static sptr make(int finish_after_ms, bool wait_inside_work);

    //! Make \p items zero-valued samples available, as a queue push would.
    virtual void inject(int items) = 0;

    //! Observations so far as one JSON object.
    virtual std::string report() const = 0;
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_M0_PROBE_SOURCE_H */
