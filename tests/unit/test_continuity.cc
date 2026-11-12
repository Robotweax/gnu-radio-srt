// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.h"

using namespace rwsrt;
using namespace rwsrt_test;

namespace {

continuity_ledger make(continuity_policy p)
{
    continuity_ledger l(p, sample_format::cf32, 100000, 1);
    l.on_connection();
    return l;
}
const auto segmented = continuity_policy::segmented_live;
const auto strict = continuity_policy::strict_stop;

} // namespace

BOOST_AUTO_TEST_SUITE(continuity)

BOOST_AUTO_TEST_CASE(clean_run_is_receiver_observed_complete)
{
    for (auto policy : { segmented, strict }) {
        auto l = make(policy);
        auto r = l.on_record(data_header(0, 0, 100, 0));
        BOOST_TEST(r.accepted);
        BOOST_TEST(!r.brk.has_value());
        r = l.on_record(data_header(0, 100, 38, 1));
        BOOST_TEST(r.accepted);
        BOOST_TEST(!r.brk.has_value());
        BOOST_TEST(l.run_open());
        r = l.on_record(end_header(0, 138, 2));
        BOOST_TEST(r.accepted);
        BOOST_TEST(r.is_end);
        BOOST_TEST(r.end_complete);
        BOOST_TEST(!l.run_open());
        BOOST_TEST(l.counters().samples_accepted == 138u);
        BOOST_TEST(l.counters().discontinuities == 0u);
    }
}

BOOST_AUTO_TEST_CASE(empty_run_is_complete)
{
    auto l = make(strict);
    const auto r = l.on_record(end_header(0, 0, 0));
    BOOST_TEST(r.accepted);
    BOOST_TEST(r.end_complete);
}

BOOST_AUTO_TEST_CASE(configuration_mismatch_is_terminal_in_both_modes)
{
    for (auto policy : { segmented, strict }) {
        auto l = make(policy);
        record_header h = data_header(0, 0, 10, 0);
        h.rate_num = 48000;
        auto r = l.on_record(h);
        BOOST_TEST(!r.accepted);
        BOOST_TEST((r.failure == ledger_failure::config_mismatch));
        // Sticky: a later valid record is not accepted either.
        r = l.on_record(data_header(0, 0, 10, 0));
        BOOST_TEST((r.failure == ledger_failure::config_mismatch));

        auto l2 = make(policy);
        h = data_header(0, 0, 10, 0);
        h.format = sample_format::f32;
        BOOST_TEST((l2.on_record(h).failure == ledger_failure::config_mismatch));
    }
}

BOOST_AUTO_TEST_CASE(initial_join_mid_run)
{
    auto l = make(segmented);
    auto r = l.on_record(data_header(0, 5000, 100, 50));
    BOOST_REQUIRE(r.accepted);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_initial_join);
    BOOST_TEST(!r.brk->has_previous);
    BOOST_TEST(!r.brk->missing_known);
    BOOST_TEST(r.brk->next.sample_index == 5000u);
    // The rest arrives intact, but the receiver never saw the beginning.
    r = l.on_record(end_header(0, 5100, 51));
    BOOST_TEST(r.accepted);
    BOOST_TEST(!r.end_complete);

    auto s = make(strict);
    r = s.on_record(data_header(0, 5000, 100, 50));
    BOOST_TEST(!r.accepted);
    BOOST_TEST((r.failure == ledger_failure::continuity_violation));
    BOOST_REQUIRE(r.brk.has_value()); // the failure still says what broke
    BOOST_TEST(r.brk->reasons == reason_initial_join);

    // Sequence 0 alone is not a clean start.
    auto s2 = make(strict);
    BOOST_TEST(!s2.on_record(data_header(0, 0, 100, 3)).accepted);
    auto s3 = make(strict);
    BOOST_TEST(!s3.on_record(data_header(1, 0, 100, 0)).accepted);
}

BOOST_AUTO_TEST_CASE(known_sample_gap)
{
    auto l = make(segmented);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    auto r = l.on_record(data_header(0, 350, 100, 3)); // two records lost
    BOOST_REQUIRE(r.accepted);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_sample_gap);
    BOOST_TEST(r.brk->missing_known);
    BOOST_TEST(r.brk->missing_samples == 250u);
    BOOST_TEST(r.brk->has_previous);
    BOOST_TEST(r.brk->previous.sample_index == 100u);
    BOOST_TEST(r.brk->next.sample_index == 350u);
    BOOST_TEST(l.counters().known_missing_samples == 250u);
    BOOST_TEST(l.counters().sequence_gaps == 1u);
    r = l.on_record(end_header(0, 450, 4));
    BOOST_TEST(r.accepted);
    BOOST_TEST(!r.end_complete);

    auto s = make(strict);
    BOOST_REQUIRE(s.on_record(data_header(0, 0, 100, 0)).accepted);
    r = s.on_record(data_header(0, 101, 100, 1)); // a single missing sample
    BOOST_TEST((r.failure == ledger_failure::continuity_violation));
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->missing_samples == 1u);
}

BOOST_AUTO_TEST_CASE(sequence_gap_without_sample_gap_is_only_diagnostic)
{
    auto l = make(strict);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    const auto r = l.on_record(data_header(0, 100, 100, 5));
    BOOST_TEST(r.accepted);
    BOOST_TEST(!r.brk.has_value());
    BOOST_TEST(l.counters().sequence_gaps == 1u);
}

BOOST_AUTO_TEST_CASE(unknown_upstream_gap_starts_a_segment)
{
    auto l = make(segmented);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    auto r = l.on_record(data_header(1, 0, 100, 1));
    BOOST_REQUIRE(r.accepted);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_upstream_unknown_gap);
    BOOST_TEST(!r.brk->missing_known);
    BOOST_TEST(l.counters().unknown_gaps == 1u);
    // Continues normally inside the new segment.
    r = l.on_record(data_header(1, 100, 100, 2));
    BOOST_TEST(r.accepted);
    BOOST_TEST(!r.brk.has_value());
    // Skipping whole segments is still one unknown gap.
    r = l.on_record(data_header(4, 20, 10, 9));
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_upstream_unknown_gap);

    auto s = make(strict);
    BOOST_REQUIRE(s.on_record(data_header(0, 0, 100, 0)).accepted);
    BOOST_TEST((s.on_record(data_header(1, 0, 100, 1)).failure ==
                ledger_failure::continuity_violation));
}

BOOST_AUTO_TEST_CASE(protocol_violations_fail_in_both_modes)
{
    for (auto policy : { segmented, strict }) {
        {
            auto l = make(policy); // exact duplicate
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
            const auto r = l.on_record(data_header(0, 0, 100, 0));
            BOOST_TEST((r.failure == ledger_failure::protocol_violation));
            BOOST_TEST(l.failed());
        }
        {
            auto l = make(policy); // sequence regresses
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 5)).accepted ==
                          (policy == segmented));
            if (policy == segmented)
                BOOST_TEST((l.on_record(data_header(0, 100, 100, 4)).failure ==
                            ledger_failure::protocol_violation));
        }
        {
            auto l = make(policy); // overlapping interval with a fresh sequence
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
            BOOST_TEST((l.on_record(data_header(0, 99, 100, 1)).failure ==
                        ledger_failure::protocol_violation));
        }
        {
            auto l = make(policy); // END below the expected coordinate
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
            BOOST_TEST((l.on_record(end_header(0, 99, 1)).failure ==
                        ledger_failure::protocol_violation));
        }
        {
            auto l = make(policy); // anything after END
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
            BOOST_REQUIRE(l.on_record(end_header(0, 100, 1)).accepted);
            BOOST_TEST((l.on_record(data_header(0, 100, 100, 2)).failure ==
                        ledger_failure::protocol_violation));
        }
        {
            auto l = make(policy); // run changes on the same connection
            BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
            BOOST_TEST((l.on_record(data_header(0, 0, 100, 0, 0x10)).failure ==
                        ledger_failure::protocol_violation));
        }
    }
    auto l = make(segmented); // segment regresses
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    BOOST_REQUIRE(l.on_record(data_header(2, 0, 100, 1)).accepted);
    BOOST_TEST((l.on_record(data_header(1, 0, 100, 2)).failure ==
                ledger_failure::protocol_violation));
}

BOOST_AUTO_TEST_CASE(reconnect_is_always_reported)
{
    auto l = make(segmented);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    l.on_connection();
    // Indices are adjacent, yet the reconnect is a discontinuity event.
    auto r = l.on_record(data_header(0, 100, 100, 1));
    BOOST_REQUIRE(r.accepted);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_reconnect);
    BOOST_TEST(r.brk->missing_known);
    BOOST_TEST(r.brk->missing_samples == 0u);
    BOOST_TEST(r.brk->connection_generation == 2u);
    // Reported once.
    r = l.on_record(data_header(0, 200, 100, 2));
    BOOST_TEST(!r.brk.has_value());
    // Reconnect combined with lost samples: both causes at one boundary.
    l.on_connection();
    r = l.on_record(data_header(0, 1000, 100, 9));
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == (reason_reconnect | reason_sample_gap));
    BOOST_TEST(r.brk->missing_samples == 700u);
    BOOST_TEST(reasons_to_string(r.brk->reasons) == "sample_gap,reconnect");
    r = l.on_record(end_header(0, 1100, 10));
    BOOST_TEST(!r.end_complete);

    auto s = make(strict); // defensive: strict never survives a reconnect
    BOOST_REQUIRE(s.on_record(data_header(0, 0, 100, 0)).accepted);
    s.on_connection();
    BOOST_TEST((s.on_record(data_header(0, 100, 100, 1)).failure ==
                ledger_failure::continuity_violation));
}

BOOST_AUTO_TEST_CASE(new_run_on_a_new_connection)
{
    auto l = make(segmented);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    l.on_connection();
    auto r = l.on_record(data_header(0, 0, 100, 0, 0x10));
    BOOST_REQUIRE(r.accepted);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == (reason_new_run | reason_reconnect));
    BOOST_TEST(!r.brk->missing_known);
    BOOST_TEST(r.brk->previous.run == run_of(0xa0), boost::test_tools::per_element());
    BOOST_TEST(r.brk->next.run == run_of(0x10), boost::test_tools::per_element());
    BOOST_TEST(l.counters().runs == 2u);
    // The new run was seen from its clean start, so it can be complete.
    r = l.on_record(end_header(0, 100, 1, 0x10));
    BOOST_TEST(r.end_complete);

    // After END, a new run on a new connection is new_run without reconnect.
    l.on_connection();
    r = l.on_record(data_header(0, 700, 10, 7, 0x20));
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == (reason_new_run | reason_initial_join));
    r = l.on_record(end_header(0, 710, 8, 0x20));
    BOOST_TEST(!r.end_complete);

    auto s = make(strict); // strict fails after the first accepted run
    BOOST_REQUIRE(s.on_record(data_header(0, 0, 100, 0)).accepted);
    BOOST_REQUIRE(s.on_record(end_header(0, 100, 1)).accepted);
    s.on_connection();
    BOOST_TEST((s.on_record(data_header(0, 0, 100, 0, 0x10)).failure ==
                ledger_failure::continuity_violation));
}

BOOST_AUTO_TEST_CASE(end_reveals_a_missing_tail)
{
    auto l = make(segmented);
    BOOST_REQUIRE(l.on_record(data_header(0, 0, 100, 0)).accepted);
    auto r = l.on_record(end_header(0, 138, 2)); // final 38-sample record lost
    BOOST_REQUIRE(r.accepted);
    BOOST_TEST(r.is_end);
    BOOST_TEST(!r.end_complete);
    BOOST_REQUIRE(r.brk.has_value());
    BOOST_TEST(r.brk->reasons == reason_sample_gap);
    BOOST_TEST(r.brk->missing_samples == 38u);

    auto s = make(strict);
    BOOST_REQUIRE(s.on_record(data_header(0, 0, 100, 0)).accepted);
    BOOST_TEST((s.on_record(end_header(0, 138, 2)).failure ==
                ledger_failure::continuity_violation));
}

BOOST_AUTO_TEST_CASE(expectation_tracks_the_next_sample)
{
    auto l = make(segmented);
    BOOST_TEST(!l.expectation().has_value());
    BOOST_REQUIRE(l.on_record(data_header(3, 40, 10, 8)).accepted);
    BOOST_REQUIRE(l.expectation().has_value());
    BOOST_TEST(l.expectation()->segment == 3u);
    BOOST_TEST(l.expectation()->sample_index == 50u);
}

BOOST_AUTO_TEST_SUITE_END()
