// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_time.h"

#include <boost/test/unit_test.hpp>

#include <limits>

using namespace rwsrt;

BOOST_AUTO_TEST_SUITE(sample_time)

BOOST_AUTO_TEST_CASE(offset_is_floor_of_exact_rational)
{
    std::int64_t ns = 0;
    bool exact = false;
    BOOST_REQUIRE(sample_offset_ns(100000, 100000, 1, ns, exact));
    BOOST_TEST(ns == 1000000000);
    BOOST_TEST(exact);
    BOOST_REQUIRE(sample_offset_ns(1, 3, 1, ns, exact)); // 1/3 s
    BOOST_TEST(ns == 333333333);
    BOOST_TEST(!exact);
    BOOST_REQUIRE(sample_offset_ns(3, 3, 1, ns, exact));
    BOOST_TEST(ns == 1000000000);
    BOOST_TEST(exact);
    // 1000000/3 samples per second: one sample lasts exactly 3000 ns.
    BOOST_REQUIRE(sample_offset_ns(7, 1000000, 3, ns, exact));
    BOOST_TEST(ns == 21000);
    BOOST_TEST(exact);
    BOOST_REQUIRE(sample_offset_ns(0, 48000, 1, ns, exact));
    BOOST_TEST(ns == 0);
    BOOST_TEST(exact);
}

BOOST_AUTO_TEST_CASE(no_drift_from_accumulated_rounding)
{
    // 44100 Hz has an inexact period. After one hour the derived time must
    // be exactly 3600 s, which a summed rounded period would miss.
    std::int64_t ns = 0;
    bool exact = false;
    BOOST_REQUIRE(sample_offset_ns(std::uint64_t{ 44100 } * 3600, 44100, 1, ns, exact));
    BOOST_TEST(ns == std::int64_t{ 3600 } * 1000000000);
    BOOST_TEST(exact);
    // NTSC-style rational rate 30000/1001: 30000 samples last 1001 s.
    BOOST_REQUIRE(sample_offset_ns(30000, 30000, 1001, ns, exact));
    BOOST_TEST(ns == std::int64_t{ 1001 } * 1000000000);
    BOOST_TEST(exact);
}

BOOST_AUTO_TEST_CASE(offset_rejects_invalid_rate_and_overflow)
{
    std::int64_t ns = 0;
    bool exact = false;
    BOOST_TEST(!sample_offset_ns(1, 0, 1, ns, exact));
    BOOST_TEST(!sample_offset_ns(1, 1, 0, ns, exact));
    // 2^63 samples at 1 Hz is far beyond 2^63 ns.
    BOOST_TEST(!sample_offset_ns(std::uint64_t{ 1 } << 63, 1, 1, ns, exact));
    // Widest operands must not overflow the 128-bit intermediate.
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST(!sample_offset_ns(max, 1, 0xffffffffu, ns, exact));
    // Largest representable: just under 2^63 ns at 1 GHz sample rate.
    BOOST_REQUIRE(sample_offset_ns(std::uint64_t{ 1 } << 62, 1000000000, 1, ns, exact));
    BOOST_TEST(ns == (std::int64_t{ 1 } << 62));
}

BOOST_AUTO_TEST_CASE(anchor_interpolates_and_widens_uncertainty)
{
    time_anchor a;
    capture_time t;
    BOOST_TEST(!a.at(0, 3, 1, t)); // no anchor: nothing is invented
    BOOST_REQUIRE(a.set(10, capture_time{ 1000, 50 }));
    BOOST_REQUIRE(a.at(10, 3, 1, t));
    BOOST_TEST((t == capture_time{ 1000, 50 })); // the anchor itself is unchanged
    BOOST_REQUIRE(a.at(11, 3, 1, t));
    BOOST_TEST((t == capture_time{ 1000 + 333333333, 51 })); // rounded: +1 ns
    BOOST_REQUIRE(a.at(13, 3, 1, t));
    BOOST_TEST((t == capture_time{ 1000 + 1000000000, 50 })); // exact: unchanged
    BOOST_TEST(!a.at(9, 3, 1, t)); // before the anchor
    a.clear();
    BOOST_TEST(!a.at(10, 3, 1, t));
}

BOOST_AUTO_TEST_CASE(anchor_never_reports_zero_from_nonzero_or_invents_precision)
{
    time_anchor a;
    capture_time t;
    BOOST_REQUIRE(a.set(0, capture_time{ 0, 0 }));
    BOOST_REQUIRE(a.at(1, 3, 1, t));
    BOOST_TEST(t.uncertainty_ns == 1u); // a source-claimed 0 still grows when rounded
}

BOOST_AUTO_TEST_CASE(anchor_rejects_impossible_times)
{
    time_anchor a;
    const std::int64_t max = std::numeric_limits<std::int64_t>::max();
    BOOST_TEST(!a.set(0, capture_time{ -1, 0 }));
    BOOST_TEST(!a.valid());
    BOOST_TEST(!a.set(0, capture_time{ max, 1 }));
    BOOST_TEST(!a.set(0, capture_time{ 0, std::uint64_t{ 1 } << 63 }));
    BOOST_REQUIRE(a.set(0, capture_time{ max - 10, 5 }));
    capture_time t;
    BOOST_TEST(a.at(0, 1000000000, 1, t));
    BOOST_TEST(a.at(5, 1000000000, 1, t));   // time + 5 + uncertainty 5 == max
    BOOST_TEST(!a.at(6, 1000000000, 1, t));  // would exceed the range
    // A failed set() leaves no stale anchor behind.
    BOOST_TEST(!a.set(0, capture_time{ -5, 0 }));
    BOOST_TEST(!a.at(0, 1, 1, t));
}

BOOST_AUTO_TEST_SUITE_END()
