// SPDX-License-Identifier: GPL-3.0-or-later
#include "record_assembler.h"
#include "test_support.h"

#include <limits>
#include <stdexcept>

using namespace rwsrt;
using namespace rwsrt_test;
using namespace std::chrono_literals;

namespace {

using clock_type = record_assembler::clock;
const clock_type::time_point t0 = clock_type::time_point() + 1h;

struct collector {
    std::vector<queue_entry> out;
    record_assembler::emit_fn fn()
    {
        return [this](queue_entry&& e) { out.push_back(std::move(e)); };
    }
    // Decode every emitted message with the production decoder.
    record_header header(std::size_t i, std::vector<float>* samples = nullptr) const
    {
        decoded_record r;
        BOOST_REQUIRE(decode(out[i].message, 1200, r) == wire_error::none);
        BOOST_TEST((r.header == out[i].header)); // metadata matches the bytes
        if (samples) {
            samples->resize(r.payload.size() / 4);
            unpack_f32le(r.payload, *samples);
        }
        return r.header;
    }
};

// cf32, 1200-byte limit: (1200 - 96) / 8 = 138 samples per record.
std::vector<float> ramp(std::size_t samples, std::size_t start = 0)
{
    std::vector<float> v(samples * 2);
    for (std::size_t i = 0; i < samples; ++i) {
        v[2 * i] = static_cast<float>(start + i);
        v[2 * i + 1] = -static_cast<float>(start + i);
    }
    return v;
}

} // namespace

BOOST_AUTO_TEST_SUITE(assembler)

BOOST_AUTO_TEST_CASE(record_capacity_matches_the_plan)
{
    assembler_config c;
    BOOST_TEST(record_assembler(c, run_of(1)).max_samples_per_record() == 138u);
    c.format = sample_format::f32;
    BOOST_TEST(record_assembler(c, run_of(1)).max_samples_per_record() == 276u);
    c.record_limit_bytes = 99; // room for less than one f32 sample
    BOOST_CHECK_THROW(record_assembler(c, run_of(1)), std::invalid_argument);
    c.record_limit_bytes = 100;
    BOOST_TEST(record_assembler(c, run_of(1)).max_samples_per_record() == 1u);
    c = assembler_config{};
    c.rate_den = 0;
    BOOST_CHECK_THROW(record_assembler(c, run_of(1)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(frames_full_records_and_a_partial_tail)
{
    record_assembler a(assembler_config{}, run_of(0xa0));
    collector c;
    const auto samples = ramp(300);
    std::size_t consumed = 0;
    BOOST_TEST((a.append(samples.data(), 300, t0, c.fn(), consumed) == assembler_status::ok));
    BOOST_TEST(consumed == 300u);
    BOOST_REQUIRE(c.out.size() == 2u); // 138 + 138, 24 pending
    BOOST_TEST(a.has_partial());
    BOOST_TEST((a.finish(c.fn()) == assembler_status::ok));
    BOOST_REQUIRE(c.out.size() == 4u);

    std::vector<float> got, all;
    const record_header h0 = c.header(0, &got);
    all.insert(all.end(), got.begin(), got.end());
    BOOST_TEST((h0 == data_header(0, 0, 138, 0)));
    const record_header h1 = c.header(1, &got);
    all.insert(all.end(), got.begin(), got.end());
    BOOST_TEST((h1 == data_header(0, 138, 138, 1)));
    const record_header h2 = c.header(2, &got);
    all.insert(all.end(), got.begin(), got.end());
    BOOST_TEST((h2 == data_header(0, 276, 24, 2)));
    BOOST_TEST((c.header(3) == end_header(0, 300, 3)));
    BOOST_TEST(all == samples, boost::test_tools::per_element()); // values and order
    BOOST_TEST(a.samples_consumed() == 300u);
    BOOST_TEST(a.records_assembled() == 4u);

    // After END nothing more is accepted.
    BOOST_TEST((a.append(samples.data(), 1, t0, c.fn(), consumed) == assembler_status::finished));
    BOOST_TEST(consumed == 0u);
    BOOST_TEST((a.finish(c.fn()) == assembler_status::finished));
    BOOST_TEST(c.out.size() == 4u);
}

BOOST_AUTO_TEST_CASE(empty_run_is_a_single_end)
{
    record_assembler a(assembler_config{}, run_of(0xa0));
    collector c;
    BOOST_TEST((a.finish(c.fn()) == assembler_status::ok));
    BOOST_REQUIRE(c.out.size() == 1u);
    BOOST_TEST((c.header(0) == end_header(0, 0, 0)));
}

BOOST_AUTO_TEST_CASE(rate_is_normalized_on_the_wire)
{
    assembler_config cfg;
    cfg.rate_num = 200000;
    cfg.rate_den = 2;
    record_assembler a(cfg, run_of(0xa0));
    collector c;
    BOOST_TEST((a.finish(c.fn()) == assembler_status::ok));
    BOOST_TEST(c.header(0).rate_num == 100000u);
    BOOST_TEST(c.header(0).rate_den == 1u);
}

BOOST_AUTO_TEST_CASE(flush_interval_bounds_assembly_residence)
{
    record_assembler a(assembler_config{}, run_of(0xa0)); // 2 ms
    collector c;
    const auto samples = ramp(10);
    std::size_t consumed = 0;
    BOOST_TEST(!a.flush_if_due(t0 + 1h, c.fn())); // nothing pending
    a.append(samples.data(), 5, t0, c.fn(), consumed);
    a.append(samples.data() + 10, 5, t0 + 1500us, c.fn(), consumed);
    BOOST_TEST(!a.flush_if_due(t0 + 1999us, c.fn()));
    // Due 2 ms after the FIRST sample of the partial record, not the last.
    BOOST_TEST(a.flush_if_due(t0 + 2ms, c.fn()));
    BOOST_REQUIRE(c.out.size() == 1u);
    BOOST_TEST((c.header(0) == data_header(0, 0, 10, 0)));
    BOOST_TEST(!a.has_partial());
    // The timer restarts with the next partial record.
    a.append(samples.data(), 1, t0 + 10ms, c.fn(), consumed);
    BOOST_TEST(!a.flush_if_due(t0 + 11ms, c.fn()));
    BOOST_TEST(a.flush_if_due(t0 + 12ms, c.fn()));
    BOOST_TEST((c.header(1) == data_header(0, 10, 1, 1)));
}

BOOST_AUTO_TEST_CASE(known_gap_advances_the_coordinate)
{
    record_assembler a(assembler_config{}, run_of(0xa0));
    collector c;
    const auto samples = ramp(20);
    std::size_t consumed = 0;
    a.append(samples.data(), 10, t0, c.fn(), consumed);
    BOOST_TEST((a.known_gap(1000, c.fn()) == assembler_status::ok)); // splits the record
    BOOST_REQUIRE(c.out.size() == 1u);
    a.append(samples.data() + 20, 10, t0, c.fn(), consumed);
    a.finish(c.fn());
    BOOST_TEST((c.header(0) == data_header(0, 0, 10, 0)));
    BOOST_TEST((c.header(1) == data_header(0, 1010, 10, 1)));
    BOOST_TEST((c.header(2) == end_header(0, 1020, 2)));
    BOOST_TEST(a.samples_consumed() == 20u); // lost samples were never consumed
}

BOOST_AUTO_TEST_CASE(unknown_gap_starts_a_new_segment_at_zero)
{
    record_assembler a(assembler_config{}, run_of(0xa0));
    collector c;
    const auto samples = ramp(20);
    std::size_t consumed = 0;
    a.append(samples.data(), 10, t0, c.fn(), consumed);
    BOOST_TEST((a.unknown_gap(c.fn()) == assembler_status::ok));
    a.append(samples.data() + 20, 10, t0, c.fn(), consumed);
    a.finish(c.fn());
    BOOST_TEST((c.header(0) == data_header(0, 0, 10, 0)));
    BOOST_TEST((c.header(1) == data_header(1, 0, 10, 1)));
    BOOST_TEST((c.header(2) == end_header(1, 10, 2)));
    // A gap at the very start, before any sample, still changes the segment.
    record_assembler b(assembler_config{}, run_of(0xa0));
    collector d;
    b.unknown_gap(d.fn());
    b.finish(d.fn());
    BOOST_TEST((d.header(0) == end_header(1, 0, 0)));
}

BOOST_AUTO_TEST_CASE(capture_time_splits_records_and_is_interpolated)
{
    assembler_config cfg;
    cfg.rate_num = 3; // 3 samples per second: inexact period
    cfg.rate_den = 1;
    cfg.record_limit_bytes = 96 + 2 * 8; // two cf32 samples per record
    record_assembler a(cfg, run_of(0xa0));
    collector c;
    const auto samples = ramp(8);
    std::size_t consumed = 0;
    a.append(samples.data(), 1, t0, c.fn(), consumed); // sample 0: no time known
    BOOST_TEST((a.set_capture_time(capture_time{ 5000000000, 40 }, c.fn()) ==
                assembler_status::ok)); // applies to sample 1
    a.append(samples.data() + 2, 5, t0, c.fn(), consumed); // samples 1..5
    a.finish(c.fn());
    BOOST_REQUIRE(c.out.size() == 5u);

    record_header h = c.header(0); // sample 0, sealed by the tag
    BOOST_TEST(h.sample_count == 1u);
    BOOST_TEST(!h.capture_time_valid); // no timestamp is invented backwards
    h = c.header(1); // samples 1,2: the anchor itself
    BOOST_TEST(h.first_sample_index == 1u);
    BOOST_TEST(h.capture_time_valid);
    BOOST_TEST(h.capture_time_ns == 5000000000);
    BOOST_TEST(h.capture_uncertainty_ns == 40u);
    h = c.header(2); // samples 3,4: 2 samples later = 2/3 s, rounded down
    BOOST_TEST(h.first_sample_index == 3u);
    BOOST_TEST(h.capture_time_ns == 5000000000 + 666666666);
    BOOST_TEST(h.capture_uncertainty_ns == 41u);
    h = c.header(3); // sample 5: 4/3 s, rounded once, not accumulated
    BOOST_TEST(h.capture_time_ns == 5000000000 + 1333333333);
    BOOST_TEST(h.capture_uncertainty_ns == 41u);
    BOOST_TEST(!c.header(4).capture_time_valid); // END carries no time
}

BOOST_AUTO_TEST_CASE(time_anchor_survives_known_gap_but_not_unknown_gap)
{
    assembler_config cfg;
    cfg.rate_num = 1000;
    record_assembler a(cfg, run_of(0xa0));
    collector c;
    const auto samples = ramp(4);
    std::size_t consumed = 0;
    a.set_capture_time(capture_time{ 1000000000, 10 }, c.fn());
    a.append(samples.data(), 1, t0, c.fn(), consumed);
    a.known_gap(499, c.fn()); // 499 lost: the sample clock kept running
    a.append(samples.data(), 1, t0, c.fn(), consumed);
    a.unknown_gap(c.fn());
    a.append(samples.data(), 1, t0, c.fn(), consumed);
    BOOST_TEST((a.set_capture_time(capture_time{ -1, 0 }, c.fn()) == assembler_status::bad_time));
    a.append(samples.data(), 1, t0, c.fn(), consumed);
    a.finish(c.fn());
    BOOST_TEST(c.header(0).capture_time_ns == 1000000000);
    record_header h = c.header(1);
    BOOST_TEST(h.first_sample_index == 500u);
    BOOST_TEST(h.capture_time_valid);
    BOOST_TEST(h.capture_time_ns == 1000000000 + 500000000); // 500 samples at 1 kHz
    h = c.header(2);
    BOOST_TEST(h.segment_id == 1u);
    BOOST_TEST(!h.capture_time_valid); // invalid until a new anchor arrives
    BOOST_TEST(!c.header(3).capture_time_valid); // an impossible time sets nothing
}

BOOST_AUTO_TEST_CASE(refuses_to_wrap_coordinates)
{
    record_assembler a(assembler_config{}, run_of(0xa0));
    collector c;
    const auto samples = ramp(10);
    std::size_t consumed = 0;
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST((a.known_gap(max - 5, c.fn()) == assembler_status::ok));
    BOOST_TEST((a.append(samples.data(), 10, t0, c.fn(), consumed) ==
                assembler_status::exhausted));
    BOOST_TEST(consumed == 5u); // only what can be addressed is consumed
    BOOST_TEST((a.known_gap(1, c.fn()) == assembler_status::exhausted));
    // What was consumed is still delivered, and the run can still end.
    BOOST_TEST((a.finish(c.fn()) == assembler_status::ok));
    BOOST_REQUIRE(c.out.size() == 2u);
    BOOST_TEST(c.header(0).first_sample_index == max - 5);
    BOOST_TEST(c.header(0).sample_count == 5u);
    BOOST_TEST(c.header(1).first_sample_index == max);
}

BOOST_AUTO_TEST_SUITE_END()
