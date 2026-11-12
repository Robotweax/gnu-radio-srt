// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RWSRT_TEST_SUPPORT_H
#define RWSRT_TEST_SUPPORT_H

#include "bounded_record_queue.h"
#include "continuity.h"
#include "wire_codec.h"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace rwsrt_test {

inline std::vector<std::uint8_t> read_fixture(const std::string& name)
{
    std::ifstream f(std::string(RWSRT_FIXTURE_DIR) + "/" + name, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(f.good(), "cannot open fixture " + name);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>());
}

inline rwsrt::run_id_t run_of(std::uint8_t first)
{
    rwsrt::run_id_t r{};
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = static_cast<std::uint8_t>(first + i);
    return r;
}

//! A valid cf32 header at 100 kS/s.
inline rwsrt::record_header data_header(std::uint64_t segment, std::uint64_t first,
                                        std::uint32_t count, std::uint64_t sequence,
                                        std::uint8_t run = 0xa0)
{
    rwsrt::record_header h;
    h.kind = rwsrt::record_kind::data;
    h.format = rwsrt::sample_format::cf32;
    h.run_id = run_of(run);
    h.segment_id = segment;
    h.first_sample_index = first;
    h.sample_count = count;
    h.rate_num = 100000;
    h.rate_den = 1;
    h.record_sequence = sequence;
    return h;
}

inline rwsrt::record_header end_header(std::uint64_t segment, std::uint64_t index,
                                       std::uint64_t sequence, std::uint8_t run = 0xa0)
{
    rwsrt::record_header h = data_header(segment, index, 0, sequence, run);
    h.kind = rwsrt::record_kind::end;
    return h;
}

//! Queue entry with a real serialized message of the right size.
inline rwsrt::queue_entry entry_of(const rwsrt::record_header& h)
{
    rwsrt::queue_entry e;
    e.header = h;
    e.message.assign(rwsrt::header_size + h.sample_count * rwsrt::sample_size(h.format), 0);
    BOOST_REQUIRE(rwsrt::encode_header(h, std::span<std::uint8_t, rwsrt::header_size>(
                                              e.message.data(), rwsrt::header_size)) ==
                  rwsrt::wire_error::none);
    return e;
}

} // namespace rwsrt_test

#endif
