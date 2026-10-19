// test_codec_battery.cpp - audit AUD-7: the Battery Level percent mapping is
// pure and host-tested in codec, so bad readings can never be broadcast as a
// real level. Contract: 0xFF = "unknown" (BLE Battery Service,
// BLE_GATT.md v1) whenever the reading is invalid or non-finite; a valid
// estimate is clamped to [0,100] and rounded half-up.
//
// Revert proof (H10): neutralizing the std::isfinite guard in
// encodeBatteryLevel() fails test_battery_nan_and_infinity_are_unknown (NaN
// would fall through to a fabricated 0..100 level); removing the !valid guard
// fails test_battery_invalid_reading_is_unknown.

#include <bw/ble/codec.h>

#include <limits>

#include <unity.h>

#include <bw/hardware.h>

using bw::BatteryReading;
using bw::ble::encodeBatteryLevel;
using bw::ble::kBatteryUnknownPercent;

namespace {

BatteryReading reading(bool valid, float percent) {
    BatteryReading r{};
    r.valid = valid;
    r.percent_estimate = percent;
    return r;
}

}  // namespace

void test_battery_invalid_reading_is_unknown(void) {
    // valid == false: the estimate is meaningless even if a number is set.
    TEST_ASSERT_EQUAL_UINT8(kBatteryUnknownPercent, encodeBatteryLevel(reading(false, 42.0f)));
    TEST_ASSERT_EQUAL_UINT8(kBatteryUnknownPercent, encodeBatteryLevel(reading(false, 0.0f)));
    // 0xFF, NOT 0: broadcasting "empty battery" from a missing divider would
    // be fabricated data (00 -1 / A-04).
    TEST_ASSERT_EQUAL_HEX8(0xFF, encodeBatteryLevel(reading(false, 99.0f)));
}

void test_battery_nan_and_infinity_are_unknown(void) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float pinf = std::numeric_limits<float>::infinity();
    const float ninf = -std::numeric_limits<float>::infinity();

    // A non-finite estimate is bad data, not a percentage: 0xFF unknown.
    TEST_ASSERT_EQUAL_UINT8(kBatteryUnknownPercent, encodeBatteryLevel(reading(true, nan)));
    TEST_ASSERT_EQUAL_UINT8(kBatteryUnknownPercent, encodeBatteryLevel(reading(true, pinf)));
    TEST_ASSERT_EQUAL_UINT8(kBatteryUnknownPercent, encodeBatteryLevel(reading(true, ninf)));
    // Sanity: the valid range still maps normally next to the rejects.
    TEST_ASSERT_EQUAL_UINT8(50, encodeBatteryLevel(reading(true, 50.0f)));
}

void test_battery_rounds_and_clamps(void) {
    TEST_ASSERT_EQUAL_UINT8(42, encodeBatteryLevel(reading(true, 42.4f)));
    TEST_ASSERT_EQUAL_UINT8(43, encodeBatteryLevel(reading(true, 42.5f)));  // half up
    TEST_ASSERT_EQUAL_UINT8(0, encodeBatteryLevel(reading(true, 0.0f)));
    TEST_ASSERT_EQUAL_UINT8(0, encodeBatteryLevel(reading(true, -5.0f)));  // clamped, not wrapped
    TEST_ASSERT_EQUAL_UINT8(100,
                            encodeBatteryLevel(reading(true, 150.0f)));  // never wraps to 0/0xFF
    TEST_ASSERT_EQUAL_UINT8(100, encodeBatteryLevel(reading(true, 99.6f)));
}
