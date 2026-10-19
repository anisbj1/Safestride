// test_codec_hr.cpp - spec 9 row 2: UINT8/UINT16 format flag, refusal on
// invalid/non-positive/non-finite bpm, round-trip values.

#include <bw/ble/codec.h>

#include <limits>

#include <unity.h>

using bw::PpgFeatures;
using bw::ble::decodeHeartRate;
using bw::ble::encodeHeartRate;
using bw::ble::HeartRateValue;

namespace {

PpgFeatures validHr(float bpm) {
    PpgFeatures p{};
    p.quality = bw::DataQuality::Good;
    p.hr_valid = true;
    p.hr_bpm = bpm;
    return p;
}

}  // namespace

void test_hr_round_trip_uint8(void) {
    uint8_t buf[8] = {};
    const bw::Result<size_t> written = encodeHeartRate(validHr(72.0f), buf, sizeof(buf));
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT(2, written.value);  // flags + 1 byte bpm
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);      // UINT8 format flag
    TEST_ASSERT_EQUAL_HEX8(72, buf[1]);

    const bw::Result<HeartRateValue> got = decodeHeartRate(buf, written.value);
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_FALSE(got.value.wide);
    TEST_ASSERT_EQUAL_UINT16(72, got.value.bpm);
}

void test_hr_round_trip_uint16(void) {
    uint8_t buf[8] = {};
    const bw::Result<size_t> written = encodeHeartRate(validHr(300.2f), buf, sizeof(buf));
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT(3, written.value);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[0]);  // UINT16 format flag set

    const bw::Result<HeartRateValue> got = decodeHeartRate(buf, written.value);
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_TRUE(got.value.wide);
    TEST_ASSERT_EQUAL_UINT16(300, got.value.bpm);  // rounded
}

void test_hr_refuses_invalid(void) {
    uint8_t buf[8] = {};

    PpgFeatures invalid = validHr(72.0f);
    invalid.hr_valid = false;  // not worn / poor signal: send NOTHING
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeHeartRate(invalid, buf, sizeof(buf)).err));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeHeartRate(validHr(0.0f), buf, sizeof(buf)).err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeHeartRate(validHr(-40.0f), buf, sizeof(buf)).err));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(bw::Err::InvalidArg),
        static_cast<int>(
            encodeHeartRate(validHr(std::numeric_limits<float>::quiet_NaN()), buf, sizeof(buf))
                .err));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(bw::Err::InvalidArg),
        static_cast<int>(
            encodeHeartRate(validHr(std::numeric_limits<float>::infinity()), buf, sizeof(buf))
                .err));
}

void test_hr_encode_rejects_small_cap(void) {
    uint8_t buf[8] = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeHeartRate(validHr(72.0f), buf, 1).err));
    // wide format needs 3 bytes
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeHeartRate(validHr(300.0f), buf, 2).err));
}

void test_hr_decode_rejects_foreign_flags(void) {
    uint8_t wide[3] = {0x01, 72, 0};     // our UINT16 image
    uint8_t rr_flag[3] = {0x02, 72, 0};  // RR-interval bit: never ours
    uint8_t short_wide[2] = {0x01, 72};  // wide flag, missing byte
    uint8_t too_short[1] = {0x00};

    TEST_ASSERT_FALSE(decodeHeartRate(rr_flag, 3).ok());
    TEST_ASSERT_FALSE(decodeHeartRate(short_wide, 2).ok());
    TEST_ASSERT_FALSE(decodeHeartRate(too_short, 1).ok());
    TEST_ASSERT_FALSE(decodeHeartRate(nullptr, 2).ok());
    TEST_ASSERT_TRUE(decodeHeartRate(wide, 3).ok());
}
