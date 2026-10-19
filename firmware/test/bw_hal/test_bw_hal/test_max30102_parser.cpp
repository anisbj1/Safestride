// MAX30102 FIFO parser tests (F01 §9: byte fixtures — wrap-around, 18-bit
// masking, saturation flag; plus timestamp reconstruction and input validation).

#include <unity.h>

#include <bw/hal/max30102_parser.h>

using bw::Err;
using bw::hal::kPpgAdcFullScaleCounts;
using bw::hal::kPpgFifoBytesPerSample;
using bw::hal::parsePpgFifo;
using bw::hal::ppgFifoSampleCount;

namespace {
constexpr uint16_t kRateHz = 100;  // F01 §8 kPpgSampleRateHz
}

void test_ppg_parser_empty_input_yields_zero_samples(void) {
    bw::PpgSample out[4];
    const bw::Result<size_t> r = parsePpgFifo(nullptr, 0, 1000, kRateHz, out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(0, r.value);
}

void test_ppg_parser_decodes_msb_first_sample(void) {
    // red = 0x010203, ir = 0x030405 (3 bytes each, MSB first, 6 bytes total;
    // both within the 18-bit window 0x000000..0x03FFFF).
    const uint8_t bytes[] = {0x01, 0x02, 0x03, 0x03, 0x04, 0x05};
    bw::PpgSample out[1];
    const bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 1000, kRateHz, out, 1);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(1, r.value);
    TEST_ASSERT_EQUAL_UINT32(0x010203, out[0].red);
    TEST_ASSERT_EQUAL_UINT32(0x030405, out[0].ir);
    TEST_ASSERT_EQUAL_UINT64(1000, out[0].t_ms);  // single sample = newest
    TEST_ASSERT_EQUAL(false, out[0].saturated);
}

void test_ppg_parser_masks_18_bit_and_flags_full_scale(void) {
    // 0xFF,0xFF,0xFF -> 0x3FFFF = full scale -> saturated on red.
    const uint8_t at_scale[] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01};
    // 0x04,0x00,0x00 = bit 18 set: outside the 18-bit window -> masked to 0.
    const uint8_t above_scale[] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    bw::PpgSample out[1];

    TEST_ASSERT_EQUAL_UINT(1, parsePpgFifo(at_scale, sizeof(at_scale), 100, kRateHz, out, 1).value);
    TEST_ASSERT_EQUAL_UINT32(kPpgAdcFullScaleCounts, out[0].red);
    TEST_ASSERT_EQUAL(true, out[0].saturated);

    TEST_ASSERT_EQUAL_UINT(
        1, parsePpgFifo(above_scale, sizeof(above_scale), 100, kRateHz, out, 1).value);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].red);
    TEST_ASSERT_EQUAL(false, out[0].saturated);
}

void test_ppg_parser_ir_full_scale_alone_saturates(void) {
    const uint8_t bytes[] = {0x00, 0x00, 0x00, 0x03, 0xFF, 0xFF};
    bw::PpgSample out[1];
    const bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 100, kRateHz, out, 1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL(true, out[0].saturated);
    TEST_ASSERT_EQUAL_UINT32(kPpgAdcFullScaleCounts, out[0].ir);
}

void test_ppg_parser_reconstructs_timestamps_backwards(void) {
    // Three samples in FIFO order (oldest first): 10 ms apart at 100 Hz.
    const uint8_t bytes[3 * kPpgFifoBytesPerSample] = {};
    bw::PpgSample out[3];
    const bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 1000, kRateHz, out, 3);

    TEST_ASSERT_EQUAL_UINT(3, r.value);
    TEST_ASSERT_EQUAL_UINT64(980, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(990, out[1].t_ms);
    TEST_ASSERT_EQUAL_UINT64(1000, out[2].t_ms);
}

void test_ppg_parser_timestamps_clamp_at_boot(void) {
    // newest_t_ms = 15 but the oldest sample would be 20 ms older: clamp to 0
    // (monotonic time starts at boot; underflow must not wrap to huge values).
    const uint8_t bytes[3 * kPpgFifoBytesPerSample] = {};
    bw::PpgSample out[3];
    const bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 15, kRateHz, out, 3);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(3, r.value);

    TEST_ASSERT_EQUAL_UINT64(0, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(5, out[1].t_ms);
    TEST_ASSERT_EQUAL_UINT64(15, out[2].t_ms);
}

void test_ppg_parser_cap_keeps_absolute_timestamps(void) {
    // 4 samples available, output room for 2: oldest two are parsed, still
    // anchored to the newest sample's time (global index, not output index).
    const uint8_t bytes[4 * kPpgFifoBytesPerSample] = {};
    bw::PpgSample out[4];
    const bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 1000, kRateHz, out, 2);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(2, r.value);
    TEST_ASSERT_EQUAL_UINT64(970, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(980, out[1].t_ms);
}

void test_ppg_parser_rejects_invalid_inputs(void) {
    const uint8_t bytes[2 * kPpgFifoBytesPerSample] = {};
    bw::PpgSample out[2];

    // Zero rate would make the reconstruction spacing undefined.
    bw::Result<size_t> r = parsePpgFifo(bytes, sizeof(bytes), 1000, 0, out, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), static_cast<int>(r.err));

    // Not a whole number of 6-byte samples: caller bug, never a silent guess.
    r = parsePpgFifo(bytes, sizeof(bytes) - 1, 1000, kRateHz, out, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), static_cast<int>(r.err));
}

void test_ppg_fifo_pointer_wrap_around(void) {
    TEST_ASSERT_EQUAL_UINT(4, ppgFifoSampleCount(2, 30));  // wrapped
    TEST_ASSERT_EQUAL_UINT(5, ppgFifoSampleCount(5, 0));
    TEST_ASSERT_EQUAL_UINT(0, ppgFifoSampleCount(0, 0));    // empty (full == empty
    TEST_ASSERT_EQUAL_UINT(0, ppgFifoSampleCount(7, 7));    // ambiguity -> OVF flag)
    TEST_ASSERT_EQUAL_UINT(31, ppgFifoSampleCount(4, 5));   // one below full
    TEST_ASSERT_EQUAL_UINT(31, ppgFifoSampleCount(31, 0));  // top index, no wrap
}

void test_ppg_fifo_pointer_masks_upper_bits(void) {
    // Registers are 5-bit; a stray upper bit must not invent samples.
    TEST_ASSERT_EQUAL_UINT(4, ppgFifoSampleCount(0x22, 0x1E));  // 2 & 31, 30 & 31
}
