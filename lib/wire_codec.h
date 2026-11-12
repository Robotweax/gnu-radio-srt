/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_WIRE_CODEC_H
#define INCLUDED_RWSRT_WIRE_CODEC_H

// RWSR version 1 record codec. Normative description: docs/protocol-v1.md.
// Pure C++: no GNU Radio and no socket dependency.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rwsrt {

inline constexpr std::size_t header_size = 96;
inline constexpr std::uint8_t wire_version = 1;

enum class record_kind : std::uint8_t { data = 1, end = 2 };
enum class sample_format : std::uint8_t { f32 = 1, cf32 = 2 };

//! Bytes of one sample on the wire.
constexpr std::size_t sample_size(sample_format f) noexcept
{
    return f == sample_format::f32 ? 4 : 8;
}

using run_id_t = std::array<std::uint8_t, 16>;

//! Decoded form of the fixed header. Reserved fields and lengths that are
//! implied by the others are not represented.
struct record_header {
    record_kind kind = record_kind::data;
    sample_format format = sample_format::cf32;
    run_id_t run_id{};
    std::uint64_t segment_id = 0;
    std::uint64_t first_sample_index = 0; //!< END: exclusive end coordinate
    std::uint32_t sample_count = 0;       //!< DATA: positive; END: zero
    std::uint32_t rate_num = 0;           //!< samples per second, normalized
    std::uint32_t rate_den = 0;
    bool capture_time_valid = false;       //!< implies clock domain Unix ns
    std::int64_t capture_time_ns = 0;      //!< first payload sample
    std::uint64_t capture_uncertainty_ns = 0;
    std::uint64_t record_sequence = 0;

    bool operator==(const record_header&) const = default;
};

enum class wire_error {
    none = 0,
    too_short,               //!< fewer bytes than a header
    too_long,                //!< larger than the caller's message bound
    bad_magic,
    unsupported_version,
    bad_header_length,
    unsupported_kind,
    unsupported_flags,
    unsupported_format,
    unsupported_clock_domain,
    reserved_not_zero,
    length_mismatch,         //!< message length != 96 + payload_bytes
    payload_mismatch,        //!< payload_bytes != sample_count * sample size
    empty_data,              //!< DATA with zero samples
    end_not_empty,           //!< END with samples, payload or a timestamp
    invalid_rate,            //!< zero numerator or denominator
    rate_not_normalized,
    time_without_flag,       //!< time fields set while the valid flag is clear
    invalid_time,            //!< flag set with wrong domain or impossible time
    coordinate_overflow,     //!< first_sample_index + sample_count wraps
    sequence_exhausted       //!< record_sequence at its maximum value
};

const char* to_string(wire_error e) noexcept;

//! Checks every rule that does not depend on message bytes. Shared by the
//! encoder and the decoder, so neither can emit or accept what the other
//! would reject.
wire_error validate(const record_header& h) noexcept;

//! Writes the 96-byte header. Returns the first violated rule, in which case
//! \p out is untouched. payload_bytes on the wire is derived from the header.
wire_error encode_header(const record_header& h,
                         std::span<std::uint8_t, header_size> out) noexcept;

struct decoded_record {
    record_header header;
    std::span<const std::uint8_t> payload; //!< view into the decoded message
};

//! Decodes one complete SRT message. \p max_message_bytes is the configured
//! record limit; it is enforced before any field is interpreted. On error
//! \p out is unspecified. Never reads outside \p message, never allocates.
wire_error decode(std::span<const std::uint8_t> message,
                  std::size_t max_message_bytes,
                  decoded_record& out) noexcept;

//! Explicit little-endian IEEE-754 binary32 conversion that preserves every
//! bit pattern (signed zero, NaN payloads). out.size() must be 4 * in.size().
void pack_f32le(std::span<const float> in, std::span<std::uint8_t> out) noexcept;
//! in.size() must be 4 * out.size().
void unpack_f32le(std::span<const std::uint8_t> in, std::span<float> out) noexcept;

//! Divides both by their GCD. Zero inputs are returned unchanged (invalid).
void normalize_rate(std::uint32_t& num, std::uint32_t& den) noexcept;

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_WIRE_CODEC_H */
