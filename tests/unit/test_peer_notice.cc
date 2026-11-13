// SPDX-License-Identifier: GPL-3.0-or-later
#include "peer_notice.h"
#include "wire_codec.h"

#include <boost/test/unit_test.hpp>

using namespace rwsrt;

BOOST_AUTO_TEST_SUITE(notice)

BOOST_AUTO_TEST_CASE(round_trip_keeps_reason_and_detail)
{
    for (auto reason : { notice_reason::configuration, notice_reason::protocol,
                         notice_reason::continuity, notice_reason::queue_overflow }) {
        const peer_notice in{ reason, "sample rate differs from configuration" };
        const auto bytes = encode(in);
        BOOST_TEST(bytes.size() == notice_header_size + in.detail.size());
        peer_notice out;
        BOOST_REQUIRE(decode(bytes, out));
        BOOST_TEST((out.reason == reason));
        BOOST_TEST(out.detail == in.detail);
    }
}

BOOST_AUTO_TEST_CASE(fixed_bytes)
{
    // The layout is part of the protocol: "RWSN", version, reason, length.
    const auto bytes = encode(peer_notice{ notice_reason::continuity, "ab" });
    const std::vector<std::uint8_t> expected{ 'R', 'W', 'S', 'N', 1, 3, 0, 2, 'a', 'b' };
    BOOST_TEST(bytes == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(detail_is_bounded_and_printable)
{
    peer_notice in{ notice_reason::protocol, std::string(1000, 'x') };
    in.detail[3] = '\n';
    in.detail[4] = '\0';
    const auto bytes = encode(in);
    BOOST_TEST(bytes.size() == notice_header_size + notice_max_detail);
    peer_notice out;
    BOOST_REQUIRE(decode(bytes, out));
    BOOST_TEST(out.detail.size() == notice_max_detail);
    BOOST_TEST(out.detail.substr(0, 6) == "xxx??x");
}

BOOST_AUTO_TEST_CASE(decoder_rejects_everything_else)
{
    const auto good = encode(peer_notice{ notice_reason::configuration, "detail" });
    peer_notice out;
    for (std::size_t cut = 0; cut < good.size(); ++cut) // every truncation
        BOOST_TEST(!decode(std::span<const std::uint8_t>(good.data(), cut), out));
    auto longer = good;
    longer.push_back('x'); // length field no longer matches
    BOOST_TEST(!decode(longer, out));
    for (std::size_t i : { std::size_t(0), std::size_t(4) }) { // magic, version
        auto bad = good;
        bad[i] ^= 0x01;
        BOOST_TEST(!decode(bad, out));
    }
    for (std::uint8_t reason : { std::uint8_t(0), std::uint8_t(5), std::uint8_t(255) }) {
        auto bad = good;
        bad[5] = reason;
        BOOST_TEST(!decode(bad, out));
    }
    std::vector<std::uint8_t> huge(notice_header_size + notice_max_detail + 1, 'x');
    BOOST_TEST(!decode(huge, out));
}

BOOST_AUTO_TEST_CASE(a_notice_is_never_a_record_and_a_record_never_a_notice)
{
    const auto bytes = encode(peer_notice{ notice_reason::protocol, std::string(150, 'y') });
    decoded_record rec;
    BOOST_TEST((decode(bytes, 1200, rec) != wire_error::none));

    record_header h;
    h.kind = record_kind::end;
    h.rate_num = 100000;
    h.rate_den = 1;
    std::array<std::uint8_t, header_size> raw{};
    BOOST_REQUIRE((encode_header(h, raw) == wire_error::none));
    peer_notice out;
    BOOST_TEST(!decode(std::span<const std::uint8_t>(raw.data(), raw.size()), out));
}

BOOST_AUTO_TEST_CASE(only_failures_caused_by_the_stream_are_reported)
{
    peer_notice n;
    BOOST_TEST(notice_for(session_failure::configuration, "d", n));
    BOOST_TEST(notice_for(session_failure::continuity, "d", n));
    BOOST_TEST(notice_for(session_failure::protocol, "d", n));
    BOOST_TEST(notice_for(session_failure::queue_overflow, "d", n));
    for (auto f : { session_failure::none, session_failure::startup_timeout,
                    session_failure::reconnect_timeout, session_failure::connection_lost,
                    session_failure::credentials, session_failure::transport,
                    session_failure::internal })
        BOOST_TEST(!notice_for(f, "d", n));
    BOOST_TEST((to_session_failure(notice_reason::continuity) == session_failure::continuity));
}

BOOST_AUTO_TEST_SUITE_END()
