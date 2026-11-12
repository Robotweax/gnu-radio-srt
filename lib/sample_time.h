/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_SAMPLE_TIME_H
#define INCLUDED_RWSRT_SAMPLE_TIME_H

// Capture-time arithmetic over a rational sample rate (docs/protocol-v1.md,
// "Capture time"). Pure integer arithmetic, no floating point.

#include <cstdint>

namespace rwsrt {

struct capture_time {
    std::int64_t time_ns = 0;
    std::uint64_t uncertainty_ns = 0;
    bool operator==(const capture_time&) const = default;
};

//! floor(samples * 1e9 * rate_den / rate_num). Returns false when the rate
//! is invalid or the result does not fit a signed 64-bit nanosecond count.
//! \p exact tells whether the division had no remainder.
bool sample_offset_ns(std::uint64_t samples,
                      std::uint32_t rate_num,
                      std::uint32_t rate_den,
                      std::int64_t& offset_ns,
                      bool& exact) noexcept;

//! One known (sample index, time) pair within a segment.
class time_anchor
{
public:
    bool valid() const noexcept { return d_valid; }
    void clear() noexcept { d_valid = false; }

    //! Returns false, leaving the anchor invalid, for an impossible time
    //! (negative, or time + uncertainty beyond the signed 64-bit range).
    bool set(std::uint64_t sample_index, capture_time t) noexcept;

    //! Time of \p sample_index, which must not precede the anchor. The
    //! uncertainty grows by one nanosecond when the offset was rounded.
    //! Returns false when there is no anchor or the result would overflow.
    bool at(std::uint64_t sample_index,
            std::uint32_t rate_num,
            std::uint32_t rate_den,
            capture_time& out) const noexcept;

private:
    bool d_valid = false;
    std::uint64_t d_index = 0;
    capture_time d_time;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_SAMPLE_TIME_H */
