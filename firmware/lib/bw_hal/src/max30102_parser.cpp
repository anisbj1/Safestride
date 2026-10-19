#include <bw/hal/max30102_parser.h>

namespace bw::hal {

namespace {
// [DERIVED] Unit conversion: TimeMs is milliseconds (02 §1), so reconstruction
// spacing in ms = 1000 ms per second / rate in Hz (10 ms at 100 Hz).
constexpr bw::TimeMs kMsPerSecond = 1000;
}  // namespace

size_t ppgFifoSampleCount(uint8_t write_ptr, uint8_t read_ptr) {
    // Registers are 5-bit by construction (0..31); mask rather than reject so a
    // stray upper bit from the bus cannot invent a huge count.
    const uint8_t wr = static_cast<uint8_t>(write_ptr & (kPpgFifoDepth - 1));
    const uint8_t rd = static_cast<uint8_t>(read_ptr & (kPpgFifoDepth - 1));
    return static_cast<size_t>((wr + kPpgFifoDepth - rd) & (kPpgFifoDepth - 1));
}

bw::Result<size_t> parsePpgFifo(const uint8_t* bytes, size_t byte_count, bw::TimeMs newest_t_ms,
                                uint16_t sample_rate_hz, bw::PpgSample* out, size_t max_out) {
    if (byte_count == 0) {
        return bw::Result<size_t>{bw::Err::Ok, 0};
    }
    if (bytes == nullptr || out == nullptr || sample_rate_hz == 0 ||
        (byte_count % kPpgFifoBytesPerSample) != 0) {
        return bw::Result<size_t>{bw::Err::InvalidArg, 0};
    }

    const size_t total = byte_count / kPpgFifoBytesPerSample;
    const size_t count = (total < max_out) ? total : max_out;

    // [DERIVED] Reconstruction spacing from the configured sample rate (F01 §8,
    // kPpgSampleRateHz): at 100 Hz this is exactly 10 ms. Rates above 1 kHz
    // cannot be spaced in millisecond TimeMs — clamp to 1 ms rather than emit
    // duplicate timestamps (unknown spacing beats wrong spacing).
    bw::TimeMs interval_ms = kMsPerSecond / sample_rate_hz;
    if (interval_ms == 0) {
        interval_ms = 1;
    }

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* s = bytes + i * kPpgFifoBytesPerSample;
        const uint32_t red_raw = (static_cast<uint32_t>(s[0]) << 16) |
                                 (static_cast<uint32_t>(s[1]) << 8) | static_cast<uint32_t>(s[2]);
        const uint32_t ir_raw = (static_cast<uint32_t>(s[3]) << 16) |
                                (static_cast<uint32_t>(s[4]) << 8) | static_cast<uint32_t>(s[5]);
        const uint32_t red = red_raw & kPpgAdcFullScaleCounts;
        const uint32_t ir = ir_raw & kPpgAdcFullScaleCounts;

        // Anchor by global index i so the cap (max_out < total) keeps absolute
        // timestamps correct for the oldest samples that were parsed.
        const uint64_t age_ms = static_cast<uint64_t>(total - 1 - i) * interval_ms;
        const bw::TimeMs t_ms =
            (age_ms <= newest_t_ms) ? newest_t_ms - static_cast<bw::TimeMs>(age_ms) : 0;

        bw::PpgSample sample{};
        sample.t_ms = t_ms;
        sample.red = red;
        sample.ir = ir;
        sample.saturated = (red == kPpgAdcFullScaleCounts) || (ir == kPpgAdcFullScaleCounts);
        out[i] = sample;
    }
    return bw::Result<size_t>{bw::Err::Ok, count};
}

}  // namespace bw::hal
