// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz target: the RWSR decoder on arbitrary bytes.
// Invariants: no out-of-bounds read (ASan), and anything accepted re-encodes
// to exactly the header bytes it came from, so the decoder accepts no message
// the encoder could not have produced.

#include "peer_notice.h"
#include "wire_codec.h"

#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace rwsrt;
    // First byte picks the message bound so both sides of it are explored.
    if (size == 0)
        return 0;
    const std::size_t bound = data[0] < 128 ? 1200 : 96 + data[0];
    const std::span<const std::uint8_t> message(data + 1, size - 1);

    // The receiver-notice decoder sees the same bytes. Anything it accepts
    // is bounded, printable, and survives a round trip unchanged.
    peer_notice notice;
    if (decode(message, notice)) {
        if (notice.detail.size() > notice_max_detail ||
            message.size() != notice_header_size + notice.detail.size())
            std::abort();
        for (char c : notice.detail)
            if (c < 0x20 || c > 0x7e)
                std::abort();
        peer_notice again_notice;
        const auto bytes = encode(notice);
        if (!decode(bytes, again_notice) || again_notice.reason != notice.reason ||
            again_notice.detail != notice.detail)
            std::abort();
    }

    decoded_record r;
    if (decode(message, bound, r) != wire_error::none)
        return 0;
    if (decode(message, notice))
        std::abort(); // one message is never both a record and a notice

    if (message.size() > bound || message.size() < header_size)
        std::abort();
    if (r.payload.data() != message.data() + header_size ||
        r.payload.size() != message.size() - header_size)
        std::abort();
    if (r.payload.size() != r.header.sample_count * sample_size(r.header.format))
        std::abort();
    if (validate(r.header) != wire_error::none)
        std::abort();

    std::uint8_t again[header_size];
    if (encode_header(r.header, std::span<std::uint8_t, header_size>(again)) != wire_error::none)
        std::abort();
    if (std::memcmp(again, message.data(), header_size) != 0)
        std::abort();
    return 0;
}
