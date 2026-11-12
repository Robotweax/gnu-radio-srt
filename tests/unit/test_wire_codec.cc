// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_support.h"

#include <cstring>
#include <limits>
#include <numeric>

using namespace rwsrt;
using namespace rwsrt_test;

namespace {

constexpr std::size_t limit = 1200;

wire_error decode_bytes(const std::vector<std::uint8_t>& m, decoded_record& out,
                        std::size_t max = limit)
{
    return decode(std::span<const std::uint8_t>(m.data(), m.size()), max, out);
}

std::vector<std::uint8_t> encode_record(const record_header& h,
                                        const std::vector<std::uint8_t>& payload = {})
{
    std::vector<std::uint8_t> m(header_size);
    BOOST_REQUIRE(encode_header(h, std::span<std::uint8_t, header_size>(m.data(), header_size)) ==
                  wire_error::none);
    m.insert(m.end(), payload.begin(), payload.end());
    return m;
}

std::vector<float> floats_from_bits(const std::vector<std::uint32_t>& bits)
{
    std::vector<float> f(bits.size());
    std::memcpy(f.data(), bits.data(), bits.size() * 4);
    return f;
}

void put32(std::vector<std::uint8_t>& m, std::size_t off, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        m[off + i] = static_cast<std::uint8_t>(v >> (8 * (3 - i)));
}
void put64(std::vector<std::uint8_t>& m, std::size_t off, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        m[off + i] = static_cast<std::uint8_t>(v >> (8 * (7 - i)));
}

} // namespace

BOOST_AUTO_TEST_SUITE(wire_codec)

// --- golden vectors: produced by tools/gen_golden_vectors.py, not by this codec

BOOST_AUTO_TEST_CASE(golden_f32_data)
{
    const auto bytes = read_fixture("f32_data.bin");
    decoded_record r;
    BOOST_REQUIRE(decode_bytes(bytes, r) == wire_error::none);
    record_header expect;
    expect.kind = record_kind::data;
    expect.format = sample_format::f32;
    expect.run_id = run_of(0x00);
    expect.sample_count = 4;
    expect.rate_num = 48000;
    expect.rate_den = 1;
    BOOST_TEST((r.header == expect));
    BOOST_TEST(r.payload.size() == 16u);

    // Bit patterns survive unpack and re-pack: +0, -0, 1.5, NaN with payload.
    std::vector<float> samples(4);
    unpack_f32le(r.payload, samples);
    const std::vector<std::uint32_t> bits = { 0x00000000u, 0x80000000u, 0x3fc00000u, 0x7fc01234u };
    BOOST_TEST(std::memcmp(samples.data(), bits.data(), 16) == 0);
    std::vector<std::uint8_t> repacked(16);
    pack_f32le(samples, repacked);
    BOOST_TEST(encode_record(expect, repacked) == bytes, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(golden_cf32_data)
{
    const auto bytes = read_fixture("cf32_data.bin");
    decoded_record r;
    BOOST_REQUIRE(decode_bytes(bytes, r) == wire_error::none);
    BOOST_TEST((r.header == data_header(2, 1000, 3, 57)));
    const auto samples = floats_from_bits(
        { 0x3f800000u, 0xbf800000u, 0x80000000u, 0x7fa00001u, 0x7f800000u, 0x00000001u });
    std::vector<std::uint8_t> payload(24);
    pack_f32le(samples, payload);
    BOOST_TEST(encode_record(r.header, payload) == bytes, boost::test_tools::per_element());
    std::vector<float> back(6);
    unpack_f32le(r.payload, back);
    BOOST_TEST(std::memcmp(back.data(), samples.data(), 24) == 0); // signalling NaN intact
}

BOOST_AUTO_TEST_CASE(golden_timed_data)
{
    const auto bytes = read_fixture("timed_data.bin");
    decoded_record r;
    BOOST_REQUIRE(decode_bytes(bytes, r) == wire_error::none);
    record_header expect = data_header(1, (std::uint64_t{ 1 } << 40) + 5, 2,
                                       (std::uint64_t{ 1 } << 33) + 7, 0);
    expect.run_id = { 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
                      0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
    expect.rate_num = 1000000;
    expect.rate_den = 3;
    expect.capture_time_valid = true;
    expect.capture_time_ns = 1794484426201407000;
    expect.capture_uncertainty_ns = 250;
    BOOST_TEST((r.header == expect));
    const std::vector<std::uint8_t> payload(bytes.begin() + header_size, bytes.end());
    BOOST_TEST(encode_record(expect, payload) == bytes, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(golden_end_records)
{
    const auto end = read_fixture("end.bin");
    decoded_record r;
    BOOST_REQUIRE(decode_bytes(end, r) == wire_error::none);
    BOOST_TEST((r.header == end_header(2, 1003, 58)));
    BOOST_TEST(r.payload.empty());
    BOOST_TEST(encode_record(r.header) == end, boost::test_tools::per_element());

    const auto empty = read_fixture("empty_run_end.bin");
    BOOST_REQUIRE(decode_bytes(empty, r) == wire_error::none);
    record_header expect = end_header(0, 0, 0, 0x00);
    expect.format = sample_format::f32;
    expect.rate_num = 48000;
    BOOST_TEST((r.header == expect));
    BOOST_TEST(encode_record(expect) == empty, boost::test_tools::per_element());
}

// --- rejection: every rule of docs/protocol-v1.md, by mutating a valid record

BOOST_AUTO_TEST_CASE(rejects_truncation_at_every_length)
{
    const auto bytes = read_fixture("cf32_data.bin");
    decoded_record r;
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + n);
        const wire_error e = decode_bytes(cut, r);
        BOOST_TEST((e == (n < header_size ? wire_error::too_short : wire_error::length_mismatch)),
                   "length " << n << " gave " << to_string(e));
    }
    auto longer = bytes;
    longer.push_back(0);
    BOOST_TEST((decode_bytes(longer, r) == wire_error::length_mismatch));
}

BOOST_AUTO_TEST_CASE(enforces_message_bound_before_parsing)
{
    std::vector<std::uint8_t> garbage(2000, 0xff);
    decoded_record r;
    BOOST_TEST((decode_bytes(garbage, r) == wire_error::too_long));
    const auto bytes = read_fixture("cf32_data.bin");
    BOOST_TEST((decode_bytes(bytes, r, bytes.size()) == wire_error::none));
    BOOST_TEST((decode_bytes(bytes, r, bytes.size() - 1) == wire_error::too_long));
}

BOOST_AUTO_TEST_CASE(rejects_bad_fixed_fields)
{
    const auto good = read_fixture("cf32_data.bin");
    decoded_record r;
    struct mutation {
        std::size_t offset;
        std::uint8_t value;
        wire_error expect;
    };
    const mutation cases[] = {
        { 0, 'X', wire_error::bad_magic },
        { 3, 'r', wire_error::bad_magic },
        { 4, 0, wire_error::unsupported_version },
        { 4, 2, wire_error::unsupported_version },
        { 5, 0, wire_error::unsupported_kind },
        { 5, 3, wire_error::unsupported_kind },
        { 6, 0x80, wire_error::unsupported_flags },
        { 7, 0x02, wire_error::unsupported_flags },
        { 9, 95, wire_error::bad_header_length },
        { 8, 1, wire_error::bad_header_length },
        { 10, 0, wire_error::unsupported_format },
        { 10, 3, wire_error::unsupported_format },
        { 11, 2, wire_error::unsupported_clock_domain },
        { 60, 1, wire_error::reserved_not_zero },
        { 63, 1, wire_error::reserved_not_zero },
        { 88, 1, wire_error::reserved_not_zero },
        { 95, 1, wire_error::reserved_not_zero },
    };
    for (const mutation& m : cases) {
        auto bad = good;
        bad[m.offset] = m.value;
        const wire_error e = decode_bytes(bad, r);
        BOOST_TEST((e == m.expect), "offset " << m.offset << " gave " << to_string(e));
    }
}

BOOST_AUTO_TEST_CASE(rejects_inconsistent_counts)
{
    const auto good = read_fixture("cf32_data.bin"); // 3 samples, 24 payload bytes
    decoded_record r;

    auto bad = good;
    put32(bad, 48, 2); // fewer samples than the payload holds
    BOOST_TEST((decode_bytes(bad, r) == wire_error::payload_mismatch));

    bad = good;
    put32(bad, 48, 0);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::empty_data));

    bad = good;
    bad[10] = 1; // f32: 3 samples would be 12 bytes, not 24
    BOOST_TEST((decode_bytes(bad, r) == wire_error::payload_mismatch));

    bad = good;
    put32(bad, 12, 25); // payload_bytes disagrees with the message length
    BOOST_TEST((decode_bytes(bad, r) == wire_error::length_mismatch));

    // sample_count * 8 wraps 32 bits to 24: must not be accepted.
    bad = good;
    put32(bad, 48, 0x20000003u);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::payload_mismatch));
}

BOOST_AUTO_TEST_CASE(rejects_invalid_end)
{
    const auto good = read_fixture("end.bin");
    decoded_record r;
    auto bad = good;
    put32(bad, 48, 1);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::end_not_empty));

    bad = good; // END with a payload
    put32(bad, 12, 8);
    bad.resize(header_size + 8);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::end_not_empty));

    bad = good; // END with a capture timestamp
    bad[7] = 1;
    bad[11] = 1;
    BOOST_TEST((decode_bytes(bad, r) == wire_error::end_not_empty));
}

BOOST_AUTO_TEST_CASE(rejects_invalid_rates)
{
    const auto good = read_fixture("cf32_data.bin");
    decoded_record r;
    auto bad = good;
    put32(bad, 52, 0);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_rate));
    bad = good;
    put32(bad, 56, 0);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_rate));
    bad = good;
    put32(bad, 56, 10); // 100000/10 is not normalized
    BOOST_TEST((decode_bytes(bad, r) == wire_error::rate_not_normalized));

    std::uint32_t n = 100000, d = 10;
    normalize_rate(n, d);
    BOOST_TEST(n == 10000u);
    BOOST_TEST(d == 1u);
    n = 30720000;
    d = 1001;
    normalize_rate(n, d);
    BOOST_TEST(std::gcd(n, d) == 1u);
}

BOOST_AUTO_TEST_CASE(rejects_impossible_timestamps)
{
    const auto timed = read_fixture("timed_data.bin");
    const auto plain = read_fixture("cf32_data.bin");
    decoded_record r;

    auto bad = plain; // time fields without the flag
    put64(bad, 64, 1);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::time_without_flag));
    bad = plain;
    put64(bad, 72, 1);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::time_without_flag));
    bad = plain; // clock domain without the flag
    bad[11] = 1;
    BOOST_TEST((decode_bytes(bad, r) == wire_error::time_without_flag));

    bad = timed; // flag without a clock domain
    bad[11] = 0;
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_time));
    bad = timed; // before the epoch
    put64(bad, 64, static_cast<std::uint64_t>(std::int64_t{ -1 }));
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_time));
    bad = timed; // uncertainty beyond the signed range
    put64(bad, 72, std::uint64_t{ 1 } << 63);
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_time));
    bad = timed; // time + uncertainty overflows
    put64(bad, 64, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    BOOST_TEST((decode_bytes(bad, r) == wire_error::invalid_time));

    auto ok = timed; // zero uncertainty at time zero is representable
    put64(ok, 64, 0);
    put64(ok, 72, 0);
    BOOST_TEST((decode_bytes(ok, r) == wire_error::none));
}

BOOST_AUTO_TEST_CASE(rejects_coordinate_and_sequence_wrap)
{
    const auto good = read_fixture("cf32_data.bin"); // 3 samples
    decoded_record r;
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();

    auto edge = good;
    put64(edge, 40, max - 3); // last sample index is max - 1: allowed
    BOOST_TEST((decode_bytes(edge, r) == wire_error::none));
    edge = good;
    put64(edge, 40, max - 2);
    BOOST_TEST((decode_bytes(edge, r) == wire_error::coordinate_overflow));

    edge = good;
    put64(edge, 80, max - 1);
    BOOST_TEST((decode_bytes(edge, r) == wire_error::none));
    edge = good;
    put64(edge, 80, max);
    BOOST_TEST((decode_bytes(edge, r) == wire_error::sequence_exhausted));
}

BOOST_AUTO_TEST_CASE(encoder_refuses_what_the_decoder_rejects)
{
    std::array<std::uint8_t, header_size> out;
    out.fill(0xee);
    record_header h = data_header(0, 0, 1, 0);
    h.rate_den = 0;
    BOOST_TEST((encode_header(h, out) == wire_error::invalid_rate));
    BOOST_TEST(out[0] == 0xee); // untouched on error
    h = data_header(0, 0, 0, 0);
    BOOST_TEST((encode_header(h, out) == wire_error::empty_data));
    h = data_header(0, 0, 1, 0);
    h.rate_num = 4;
    h.rate_den = 2;
    BOOST_TEST((encode_header(h, out) == wire_error::rate_not_normalized));
    h = data_header(0, 0, 1, 0);
    h.capture_time_ns = 5;
    BOOST_TEST((encode_header(h, out) == wire_error::time_without_flag));
    h = end_header(0, 0, 0);
    h.capture_time_valid = true;
    BOOST_TEST((encode_header(h, out) == wire_error::end_not_empty));
    h = data_header(0, 0, 0x20000000u, 0); // payload would not fit 32 bits
    BOOST_TEST((encode_header(h, out) == wire_error::payload_mismatch));
    h = data_header(0, 0, 1, 0);
    h.kind = static_cast<record_kind>(9);
    BOOST_TEST((encode_header(h, out) == wire_error::unsupported_kind));
}

BOOST_AUTO_TEST_CASE(float_conversion_is_explicit_little_endian)
{
    const float one = 1.0f; // 0x3f800000
    std::array<std::uint8_t, 4> b{};
    pack_f32le(std::span<const float>(&one, 1), b);
    BOOST_TEST(b[0] == 0x00);
    BOOST_TEST(b[1] == 0x00);
    BOOST_TEST(b[2] == 0x80);
    BOOST_TEST(b[3] == 0x3f);
    // Every byte position round-trips for a spread of patterns.
    for (std::uint32_t bits : { 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                                0x7f800001u, 0xffffffffu, 0x00000001u, 0x12345678u }) {
        float f;
        std::memcpy(&f, &bits, 4);
        pack_f32le(std::span<const float>(&f, 1), b);
        float g;
        unpack_f32le(b, std::span<float>(&g, 1));
        std::uint32_t back;
        std::memcpy(&back, &g, 4);
        BOOST_TEST(back == bits);
    }
}

BOOST_AUTO_TEST_SUITE_END()
