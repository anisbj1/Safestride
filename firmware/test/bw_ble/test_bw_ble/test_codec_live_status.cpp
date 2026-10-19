// test_codec_live_status.cpp - spec 9 row 1: Live Status round trip <= 20 B,
// version byte, every enum value, in_state saturation, confidence clamp and
// the full 32-bit reasons mask.

#include <bw/ble/codec.h>

#include <cmath>
#include <cstring>
#include <limits>

#include <unity.h>

using bw::AlertnessState;
using bw::OperatingMode;
using bw::PowerProfile;
using bw::StateSnapshot;
using bw::WearState;
using bw::ble::decodeLiveStatus;
using bw::ble::encodeLiveStatus;
using bw::ble::kGattContractVersion;
using bw::ble::kLiveStatusBytes;
using bw::ble::LiveStatus;

namespace {

StateSnapshot snapshot() {
    StateSnapshot s{};
    s.t_ms = 1000;
    s.alertness = AlertnessState::Drowsy;
    s.wear = WearState::Worn;
    s.activity = bw::ActivityContext::Sitting;
    s.quality.imu = bw::DataQuality::Good;
    s.quality.ppg = bw::DataQuality::Degraded;
    s.mode = OperatingMode::Day;
    s.power = PowerProfile::Normal;
    s.in_state_ms = 45'000;
    s.confidence = 0.5f;
    s.reasons = 0x1234'5678u;
    return s;
}

}  // namespace

// 14 B image, fits the external.md 6.2 20 B cap with room to spare.
void test_live_status_round_trip(void) {
    const StateSnapshot s = snapshot();
    uint8_t buf[32] = {};
    const bw::Result<size_t> written = encodeLiveStatus(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT(kLiveStatusBytes, written.value);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(20, written.value);  // 6.2 mandate

    LiveStatus got{};
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, written.value, got).ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::Drowsy), got.alertness);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WearState::Worn), got.wear);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::ActivityContext::Sitting), got.activity);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::DataQuality::Degraded),
                            got.quality);  // worst-of imu/ppg
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(OperatingMode::Day), got.mode);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PowerProfile::Normal), got.power);
    TEST_ASSERT_EQUAL_UINT16(45, got.in_state_s);
    TEST_ASSERT_EQUAL_UINT8(50, got.confidence_pct);
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, got.reasons);
}

void test_live_status_version_byte(void) {
    const StateSnapshot s = snapshot();
    uint8_t buf[kLiveStatusBytes] = {};
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_EQUAL_UINT8(kGattContractVersion, buf[0]);

    buf[0] = static_cast<uint8_t>(kGattContractVersion + 1);  // unknown version
    LiveStatus got{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(decodeLiveStatus(buf, sizeof(buf), got).err));
    buf[0] = kGattContractVersion;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(decodeLiveStatus(buf, sizeof(buf) - 1, got).err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(decodeLiveStatus(nullptr, sizeof(buf), got).err));
}

void test_live_status_every_enum_value(void) {
    uint8_t buf[kLiveStatusBytes] = {};
    LiveStatus got{};
    for (uint8_t v = 0; v <= static_cast<uint8_t>(AlertnessState::PossibleSleep); ++v) {
        StateSnapshot s = snapshot();
        s.alertness = static_cast<AlertnessState>(v);
        TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
        TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
        TEST_ASSERT_EQUAL_UINT8(v, got.alertness);
    }
    for (uint8_t v = 0; v <= static_cast<uint8_t>(WearState::NotWorn); ++v) {
        StateSnapshot s = snapshot();
        s.wear = static_cast<WearState>(v);
        TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
        TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
        TEST_ASSERT_EQUAL_UINT8(v, got.wear);
    }
    for (uint8_t v = 0; v <= static_cast<uint8_t>(bw::ActivityContext::InactiveProlonged); ++v) {
        StateSnapshot s = snapshot();
        s.activity = static_cast<bw::ActivityContext>(v);
        TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
        TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
        TEST_ASSERT_EQUAL_UINT8(v, got.activity);
    }
    for (uint8_t v = 0; v <= static_cast<uint8_t>(OperatingMode::Research); ++v) {
        StateSnapshot s = snapshot();
        s.mode = static_cast<OperatingMode>(v);
        TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
        TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
        TEST_ASSERT_EQUAL_UINT8(v, got.mode);
    }
    for (uint8_t v = 0; v <= static_cast<uint8_t>(PowerProfile::LowPower); ++v) {
        StateSnapshot s = snapshot();
        s.power = static_cast<PowerProfile>(v);
        TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
        TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
        TEST_ASSERT_EQUAL_UINT8(v, got.power);
    }
}

void test_live_status_in_state_saturates(void) {
    uint8_t buf[kLiveStatusBytes] = {};
    LiveStatus got{};
    StateSnapshot s = snapshot();
    s.in_state_ms = 0xFFFFull * 1000ull;  // exactly the u16 ceiling
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, got.in_state_s);

    s.in_state_ms = 0xFFFFull * 1000ull + 999ull;  // one second past it
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, got.in_state_s);  // clips, never wraps
}

void test_live_status_confidence_clamps(void) {
    uint8_t buf[kLiveStatusBytes] = {};
    LiveStatus got{};
    StateSnapshot s = snapshot();

    s.confidence = 1.5f;
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_UINT8(100, got.confidence_pct);

    s.confidence = -0.3f;
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_UINT8(0, got.confidence_pct);

    s.confidence = std::numeric_limits<float>::quiet_NaN();  // honest zero
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_UINT8(0, got.confidence_pct);
}

void test_live_status_reasons_full_32_bits(void) {
    uint8_t buf[kLiveStatusBytes] = {};
    LiveStatus got{};
    StateSnapshot s = snapshot();
    s.reasons = 0xFFFF'FFFFu;
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, got.reasons);

    s.reasons = 1u << 31;  // the top bit alone survives
    TEST_ASSERT_TRUE(encodeLiveStatus(s, buf, sizeof(buf)).ok());
    TEST_ASSERT_TRUE(decodeLiveStatus(buf, sizeof(buf), got).ok());
    TEST_ASSERT_EQUAL_HEX32(1u << 31, got.reasons);
}

void test_live_status_encode_rejects_small_cap(void) {
    uint8_t buf[kLiveStatusBytes] = {};
    const StateSnapshot s = snapshot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeLiveStatus(s, buf, kLiveStatusBytes - 1).err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg),
                          static_cast<int>(encodeLiveStatus(s, nullptr, 32).err));
}
