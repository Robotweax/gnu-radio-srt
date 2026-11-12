/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sample_time.h"

#include <limits>

namespace rwsrt {

namespace {
__extension__ typedef unsigned __int128 u128; // GCC and Clang, 64-bit targets
constexpr std::uint64_t i64_max =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

bool sample_offset_ns(std::uint64_t samples,
                      std::uint32_t rate_num,
                      std::uint32_t rate_den,
                      std::int64_t& offset_ns,
                      bool& exact) noexcept
{
    if (rate_num == 0 || rate_den == 0)
        return false;
    // samples < 2^64, 1e9 < 2^30, rate_den < 2^32: the product is below 2^126.
    const u128 numerator = static_cast<u128>(samples) * 1000000000u * rate_den;
    const u128 quotient = numerator / rate_num;
    if (quotient > i64_max)
        return false;
    offset_ns = static_cast<std::int64_t>(quotient);
    exact = (numerator % rate_num) == 0;
    return true;
}

bool time_anchor::set(std::uint64_t sample_index, capture_time t) noexcept
{
    d_valid = false;
    if (t.time_ns < 0 || t.uncertainty_ns > i64_max ||
        static_cast<std::uint64_t>(t.time_ns) > i64_max - t.uncertainty_ns)
        return false;
    d_index = sample_index;
    d_time = t;
    d_valid = true;
    return true;
}

bool time_anchor::at(std::uint64_t sample_index,
                     std::uint32_t rate_num,
                     std::uint32_t rate_den,
                     capture_time& out) const noexcept
{
    if (!d_valid || sample_index < d_index)
        return false;
    std::int64_t offset = 0;
    bool exact = false;
    if (!sample_offset_ns(sample_index - d_index, rate_num, rate_den, offset, exact))
        return false;
    const std::uint64_t time = static_cast<std::uint64_t>(d_time.time_ns);
    const std::uint64_t uoffset = static_cast<std::uint64_t>(offset);
    if (uoffset > i64_max - time)
        return false;
    const std::uint64_t uncertainty = d_time.uncertainty_ns + (exact ? 0u : 1u);
    if (uncertainty > i64_max || time + uoffset > i64_max - uncertainty)
        return false;
    out.time_ns = static_cast<std::int64_t>(time + uoffset);
    out.uncertainty_ns = uncertainty;
    return true;
}

} // namespace rwsrt
