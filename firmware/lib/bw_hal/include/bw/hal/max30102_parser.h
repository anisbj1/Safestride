#pragma once

// bw/hal/max30102_parser.h — MAX30102 FIFO byte parsing (pure; F01 §9 host
// tests: byte fixtures, wrap-around, 18-bit masking, saturation flag).
// Register-level access lives in the platform driver; this file only turns
// bytes into contract samples so it runs on the host with no hardware.

#include <cstddef>
#include <cstdint>

#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::hal {

// [HW] One FIFO sample = 6 bytes: 3 red + 3 IR, MSB first, 18 bits each
// (MAX30102 datasheet, FIFO_data register: 2 × 3 bytes per sample; 00 §3: red
// + IR only).
constexpr size_t kPpgFifoBytesPerSample = 6;

// [HW] 18-bit ADC full scale: 2^18 - 1 = 262143 counts.
constexpr uint32_t kPpgAdcFullScaleCounts = (1u << 18) - 1;

// [HW] FIFO holds 32 samples (00 §3); WR/RD pointers are 5-bit registers.
constexpr uint8_t kPpgFifoDepth = 32;
// The pointer wrap-around below uses a bitmask, which only works for a
// power-of-two depth — guard the datasheet fact against a future edit.
static_assert((kPpgFifoDepth & (kPpgFifoDepth - 1)) == 0,
              "FIFO depth must be a power of two for pointer wrap-around masking");

// Samples available between the 5-bit FIFO write/read pointers (wrap-around
// handled). Equal pointers read as "empty": a *full* FIFO with equal pointers
// is only distinguishable via the overflow flag, which the driver checks
// first (-> Err::Overflow, core.md CR-004 sense, FaultCode::PpgFifoOverrun).
size_t ppgFifoSampleCount(uint8_t write_ptr, uint8_t read_ptr);

// Parses FIFO bytes (oldest sample first — FIFO read order) into `out`, at most
// `max_out` samples. Timestamps are reconstructed backwards from
// `newest_t_ms`: sample i is (total - 1 - i) intervals old, interval =
// 1000 / sample_rate_hz ms, clamped at 0 when the arithmetic would underflow
// (boot). `saturated` marks ADC full-scale counts on either channel — a raw
// sensor property; windowed saturation analysis belongs to F23.
//
// Err::InvalidArg: sample_rate_hz == 0, byte_count is not a whole number of
// samples, or a non-empty input has null pointers. Empty input -> Ok, 0.
bw::Result<size_t> parsePpgFifo(const uint8_t* bytes, size_t byte_count, bw::TimeMs newest_t_ms,
                                uint16_t sample_rate_hz, bw::PpgSample* out, size_t max_out);

}  // namespace bw::hal
