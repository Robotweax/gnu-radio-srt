/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wire_codec.h"

#include <cstring>
#include <limits>
#include <numeric>

namespace rwsrt {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "RWSR v1 payloads are IEEE-754 binary32");

namespace {

// Field offsets of the fixed header (docs/protocol-v1.md).
constexpr std::size_t off_magic = 0;
constexpr std::size_t off_version = 4;
constexpr std::size_t off_kind = 5;
constexpr std::size_t off_flags = 6;
constexpr std::size_t off_header_bytes = 8;
constexpr std::size_t off_format = 10;
constexpr std::size_t off_clock_domain = 11;
constexpr std::size_t off_payload_bytes = 12;
constexpr std::size_t off_run_id = 16;
constexpr std::size_t off_segment_id = 32;
constexpr std::size_t off_first_sample = 40;
constexpr std::size_t off_sample_count = 48;
constexpr std::size_t off_rate_num = 52;
constexpr std::size_t off_rate_den = 56;
constexpr std::size_t off_reserved_a = 60;
constexpr std::size_t off_capture_time = 64;
constexpr std::size_t off_capture_unc = 72;
constexpr std::size_t off_sequence = 80;
constexpr std::size_t off_reserved_b = 88;

constexpr std::uint8_t magic[4] = { 'R', 'W', 'S', 'R' };
constexpr std::uint16_t flag_capture_time = 0x0001;
constexpr std::uint8_t clock_unavailable = 0;
constexpr std::uint8_t clock_unix_ns = 1;

template <typename T>
void put_be(std::uint8_t* p, T v) noexcept
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        p[i] = static_cast<std::uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
}

template <typename T>
T get_be(const std::uint8_t* p) noexcept
{
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v = static_cast<T>((v << 8) | p[i]);
    return v;
}

} // namespace

const char* to_string(wire_error e) noexcept
{
    switch (e) {
    case wire_error::none: return "none";
    case wire_error::too_short: return "too_short";
    case wire_error::too_long: return "too_long";
    case wire_error::bad_magic: return "bad_magic";
    case wire_error::unsupported_version: return "unsupported_version";
    case wire_error::bad_header_length: return "bad_header_length";
    case wire_error::unsupported_kind: return "unsupported_kind";
    case wire_error::unsupported_flags: return "unsupported_flags";
    case wire_error::unsupported_format: return "unsupported_format";
    case wire_error::unsupported_clock_domain: return "unsupported_clock_domain";
    case wire_error::reserved_not_zero: return "reserved_not_zero";
    case wire_error::length_mismatch: return "length_mismatch";
    case wire_error::payload_mismatch: return "payload_mismatch";
    case wire_error::empty_data: return "empty_data";
    case wire_error::end_not_empty: return "end_not_empty";
    case wire_error::invalid_rate: return "invalid_rate";
    case wire_error::rate_not_normalized: return "rate_not_normalized";
    case wire_error::time_without_flag: return "time_without_flag";
    case wire_error::invalid_time: return "invalid_time";
    case wire_error::coordinate_overflow: return "coordinate_overflow";
    case wire_error::sequence_exhausted: return "sequence_exhausted";
    }
    return "unknown";
}

wire_error validate(const record_header& h) noexcept
{
    if (h.kind != record_kind::data && h.kind != record_kind::end)
        return wire_error::unsupported_kind;
    if (h.format != sample_format::f32 && h.format != sample_format::cf32)
        return wire_error::unsupported_format;
    if (h.kind == record_kind::data) {
        if (h.sample_count == 0)
            return wire_error::empty_data;
    } else if (h.sample_count != 0 || h.capture_time_valid) {
        return wire_error::end_not_empty;
    }
    if (h.rate_num == 0 || h.rate_den == 0)
        return wire_error::invalid_rate;
    if (std::gcd(h.rate_num, h.rate_den) != 1)
        return wire_error::rate_not_normalized;
    if (!h.capture_time_valid) {
        if (h.capture_time_ns != 0 || h.capture_uncertainty_ns != 0)
            return wire_error::time_without_flag;
    } else {
        // Unix time before the epoch, or an uncertainty that cannot be added
        // to the timestamp, is treated as impossible in version 1.
        const auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (h.capture_time_ns < 0 || h.capture_uncertainty_ns > max ||
            static_cast<std::uint64_t>(h.capture_time_ns) > max - h.capture_uncertainty_ns)
            return wire_error::invalid_time;
    }
    if (h.first_sample_index >
        std::numeric_limits<std::uint64_t>::max() - h.sample_count)
        return wire_error::coordinate_overflow;
    if (h.record_sequence == std::numeric_limits<std::uint64_t>::max())
        return wire_error::sequence_exhausted;
    return wire_error::none;
}

wire_error encode_header(const record_header& h,
                         std::span<std::uint8_t, header_size> out) noexcept
{
    if (const wire_error e = validate(h); e != wire_error::none)
        return e;
    std::uint8_t* p = out.data();
    std::memset(p, 0, header_size);
    std::memcpy(p + off_magic, magic, 4);
    p[off_version] = wire_version;
    p[off_kind] = static_cast<std::uint8_t>(h.kind);
    put_be<std::uint16_t>(p + off_flags, h.capture_time_valid ? flag_capture_time : 0);
    put_be<std::uint16_t>(p + off_header_bytes, static_cast<std::uint16_t>(header_size));
    p[off_format] = static_cast<std::uint8_t>(h.format);
    p[off_clock_domain] = h.capture_time_valid ? clock_unix_ns : clock_unavailable;
    // sample_count <= 2^32-1 and sample size <= 8, so the product fits 64 bits;
    // a product above 2^32-1 cannot be expressed and is a payload mismatch.
    const std::uint64_t payload =
        static_cast<std::uint64_t>(h.sample_count) * sample_size(h.format);
    if (payload > std::numeric_limits<std::uint32_t>::max())
        return wire_error::payload_mismatch;
    put_be<std::uint32_t>(p + off_payload_bytes, static_cast<std::uint32_t>(payload));
    std::memcpy(p + off_run_id, h.run_id.data(), h.run_id.size());
    put_be<std::uint64_t>(p + off_segment_id, h.segment_id);
    put_be<std::uint64_t>(p + off_first_sample, h.first_sample_index);
    put_be<std::uint32_t>(p + off_sample_count, h.sample_count);
    put_be<std::uint32_t>(p + off_rate_num, h.rate_num);
    put_be<std::uint32_t>(p + off_rate_den, h.rate_den);
    put_be<std::uint64_t>(p + off_capture_time, static_cast<std::uint64_t>(h.capture_time_ns));
    put_be<std::uint64_t>(p + off_capture_unc, h.capture_uncertainty_ns);
    put_be<std::uint64_t>(p + off_sequence, h.record_sequence);
    return wire_error::none;
}

wire_error decode(std::span<const std::uint8_t> message,
                  std::size_t max_message_bytes,
                  decoded_record& out) noexcept
{
    if (message.size() < header_size)
        return wire_error::too_short;
    if (message.size() > max_message_bytes)
        return wire_error::too_long;
    const std::uint8_t* p = message.data();
    if (std::memcmp(p + off_magic, magic, 4) != 0)
        return wire_error::bad_magic;
    if (p[off_version] != wire_version)
        return wire_error::unsupported_version;
    if (get_be<std::uint16_t>(p + off_header_bytes) != header_size)
        return wire_error::bad_header_length;
    const std::uint8_t kind = p[off_kind];
    if (kind != 1 && kind != 2)
        return wire_error::unsupported_kind;
    const std::uint16_t flags = get_be<std::uint16_t>(p + off_flags);
    if ((flags & ~flag_capture_time) != 0)
        return wire_error::unsupported_flags;
    const std::uint8_t format = p[off_format];
    if (format != 1 && format != 2)
        return wire_error::unsupported_format;
    const std::uint8_t domain = p[off_clock_domain];
    if (domain != clock_unavailable && domain != clock_unix_ns)
        return wire_error::unsupported_clock_domain;
    if (get_be<std::uint32_t>(p + off_reserved_a) != 0 ||
        get_be<std::uint64_t>(p + off_reserved_b) != 0)
        return wire_error::reserved_not_zero;
    const std::uint32_t payload_bytes = get_be<std::uint32_t>(p + off_payload_bytes);
    if (message.size() - header_size != payload_bytes)
        return wire_error::length_mismatch;

    record_header& h = out.header;
    h.kind = static_cast<record_kind>(kind);
    h.format = static_cast<sample_format>(format);
    std::memcpy(h.run_id.data(), p + off_run_id, h.run_id.size());
    h.segment_id = get_be<std::uint64_t>(p + off_segment_id);
    h.first_sample_index = get_be<std::uint64_t>(p + off_first_sample);
    h.sample_count = get_be<std::uint32_t>(p + off_sample_count);
    h.rate_num = get_be<std::uint32_t>(p + off_rate_num);
    h.rate_den = get_be<std::uint32_t>(p + off_rate_den);
    h.capture_time_valid = (flags & flag_capture_time) != 0;
    h.capture_time_ns = static_cast<std::int64_t>(get_be<std::uint64_t>(p + off_capture_time));
    h.capture_uncertainty_ns = get_be<std::uint64_t>(p + off_capture_unc);
    h.record_sequence = get_be<std::uint64_t>(p + off_sequence);

    // Flag and clock domain must agree in both directions.
    if (h.capture_time_valid != (domain == clock_unix_ns))
        return h.capture_time_valid ? wire_error::invalid_time : wire_error::time_without_flag;
    if (h.kind == record_kind::end && payload_bytes != 0)
        return wire_error::end_not_empty;
    if (const wire_error e = validate(h); e != wire_error::none)
        return e;
    if (static_cast<std::uint64_t>(h.sample_count) * sample_size(h.format) != payload_bytes)
        return wire_error::payload_mismatch;
    out.payload = message.subspan(header_size);
    return wire_error::none;
}

void pack_f32le(std::span<const float> in, std::span<std::uint8_t> out) noexcept
{
    std::uint8_t* p = out.data();
    for (const float& f : in) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        p[0] = static_cast<std::uint8_t>(bits);
        p[1] = static_cast<std::uint8_t>(bits >> 8);
        p[2] = static_cast<std::uint8_t>(bits >> 16);
        p[3] = static_cast<std::uint8_t>(bits >> 24);
        p += 4;
    }
}

void unpack_f32le(std::span<const std::uint8_t> in, std::span<float> out) noexcept
{
    const std::uint8_t* p = in.data();
    for (float& f : out) {
        const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) |
                                   (static_cast<std::uint32_t>(p[1]) << 8) |
                                   (static_cast<std::uint32_t>(p[2]) << 16) |
                                   (static_cast<std::uint32_t>(p[3]) << 24);
        std::memcpy(&f, &bits, 4);
        p += 4;
    }
}

void normalize_rate(std::uint32_t& num, std::uint32_t& den) noexcept
{
    if (num == 0 || den == 0)
        return;
    const std::uint32_t g = std::gcd(num, den);
    num /= g;
    den /= g;
}

} // namespace rwsrt
