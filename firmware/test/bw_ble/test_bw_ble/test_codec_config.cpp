// test_codec_config.cpp - spec 9 row 4: dump from a fake ISettings (order,
// entry-boundary truncation, total_count) and write decode (happy, unknown
// name, type mismatch, out-of-range -> CLAMPED, never rejected).

#include <bw/ble/codec.h>

#include <cstring>

#include <unity.h>

#include "ble_test_fakes.h"

using bw::SettingKey;
using bw::SettingType;
using bw::ble::applyConfigWrite;
using bw::ble::encodeConfigDump;
using bw::ble::kGattContractVersion;
using bw_ble_test::FakeSettings;

namespace {

void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Two registered settings, registration order: bool then float.
FakeSettings twoSettings() {
    FakeSettings s;
    s.add(static_cast<SettingKey>(1), SettingType::Bool, FakeSettings::makeBool(false),
          FakeSettings::makeBool(true), FakeSettings::makeBool(false), "alert_enabled");
    s.add(static_cast<SettingKey>(2), SettingType::Float, FakeSettings::makeFloat(0.1f),
          FakeSettings::makeFloat(0.9f), FakeSettings::makeFloat(0.5f), "drowsy_thr");
    return s;
}

// Config WRITE image: [ver][type u8][value u32][name_len u16][name].
size_t buildWrite(uint8_t* out, uint8_t type, uint32_t bits, const char* name) {
    const size_t name_len = std::strlen(name);
    out[0] = kGattContractVersion;
    out[1] = type;
    putU32(&out[2], bits);
    putU16(&out[6], static_cast<uint16_t>(name_len));
    std::memcpy(&out[9], name, name_len);
    return 9 + name_len;
}

}  // namespace

void test_config_dump_order_and_totals(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t buf[256] = {};

    const bw::Result<size_t> written =
        encodeConfigDump(settings, specs, bw::ble::kMaxConfigSettings, buf, sizeof(buf));
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT8(kGattContractVersion, buf[0]);
    TEST_ASSERT_EQUAL_UINT16(2, static_cast<uint16_t>(buf[1] | (buf[2] << 8)));  // total

    size_t n = 3;
    // Entry 1: bool "alert_enabled" (registration order).
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SettingType::Bool), buf[n]);
    TEST_ASSERT_EQUAL_UINT32(0, getU32(&buf[n + 1]));  // default false
    const uint16_t name1 = static_cast<uint16_t>(buf[n + 5] | (buf[n + 6] << 8));
    TEST_ASSERT_EQUAL_UINT16(13, name1);
    TEST_ASSERT_EQUAL_STRING_LEN("alert_enabled", &buf[n + 7], name1);
    n += 7 + name1;
    // Entry 2: float "drowsy_thr", default 0.5f bit pattern.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SettingType::Float), buf[n]);
    uint32_t bits = 0;
    std::memcpy(&bits, &buf[n + 1], 4);
    float f = 0;
    std::memcpy(&f, &bits, 4);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, f);
    n += 7 + 10;
    TEST_ASSERT_EQUAL_UINT(written.value, n);  // nothing else was written
}

void test_config_dump_truncates_at_entry_boundary(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t buf[64] = {};

    // Room for the header (3 B) + exactly one entry ("alert_enabled" needs
    // 7 + 13 = 20 B => 23 total); the second entry (17 B) would not fit, so
    // it is dropped at the entry boundary instead of being split.
    const size_t cap = 23 + 4;
    const bw::Result<size_t> written =
        encodeConfigDump(settings, specs, bw::ble::kMaxConfigSettings, buf, cap);
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT(23, written.value);  // header + one whole entry
    TEST_ASSERT_EQUAL_UINT16(2, static_cast<uint16_t>(buf[1] | (buf[2] << 8)));  // true total
}

void test_config_dump_rejects_null(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t buf[64] = {};
    TEST_ASSERT_FALSE(encodeConfigDump(settings, nullptr, 4, buf, sizeof(buf)).ok());
    TEST_ASSERT_FALSE(encodeConfigDump(settings, specs, 4, nullptr, sizeof(buf)).ok());
}

void test_config_write_happy_paths(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t wire[64] = {};

    size_t len = buildWrite(wire, static_cast<uint8_t>(SettingType::Bool), 1, "alert_enabled");
    TEST_ASSERT_TRUE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len).ok());
    TEST_ASSERT_EQUAL_UINT8(1, settings.valueAt(0).b ? 1 : 0);
    TEST_ASSERT_TRUE(settings.valueAt(0).b);
    TEST_ASSERT_EQUAL_UINT32(1, settings.setCalls());

    len = buildWrite(wire, static_cast<uint8_t>(SettingType::Float), 0, "drowsy_thr");
    float f = 0.75f;
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    putU32(&wire[2], bits);
    TEST_ASSERT_TRUE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len).ok());
    TEST_ASSERT_EQUAL_FLOAT(0.75f, settings.valueAt(1).f);
}

void test_config_write_unknown_name(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t wire[64] = {};
    const size_t len = buildWrite(wire, static_cast<uint8_t>(SettingType::Bool), 1, "no_such_key");
    const bw::Result<void> r =
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT32(0, settings.setCalls());  // nothing was written
}

void test_config_write_type_mismatch(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t wire[64] = {};
    // Registered as Bool, wire claims Int: rejected, never coerced.
    const size_t len = buildWrite(wire, static_cast<uint8_t>(SettingType::Int), 1, "alert_enabled");
    TEST_ASSERT_FALSE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len).ok());
    TEST_ASSERT_EQUAL_UINT32(0, settings.setCalls());
}

void test_config_write_out_of_range_clamps(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t wire[64] = {};
    // The registered float range is 0.1..0.9: 5.0f CLAMPS, never rejects
    // (services.md 5 - the fake honors the same contract as F18).
    float f = 5.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const size_t len =
        buildWrite(wire, static_cast<uint8_t>(SettingType::Float), bits, "drowsy_thr");
    TEST_ASSERT_TRUE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len).ok());
    TEST_ASSERT_EQUAL_FLOAT(0.9f, settings.valueAt(1).f);
}

void test_config_write_version_and_length(void) {
    FakeSettings settings = twoSettings();
    bw::SettingSpec specs[bw::ble::kMaxConfigSettings] = {};
    uint8_t wire[64] = {};
    size_t len = buildWrite(wire, static_cast<uint8_t>(SettingType::Bool), 1, "alert_enabled");

    wire[0] = static_cast<uint8_t>(kGattContractVersion + 1);  // unknown version
    TEST_ASSERT_FALSE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire, len).ok());

    wire[0] = kGattContractVersion;
    TEST_ASSERT_FALSE(applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, wire,
                                       len - 1)  // name_len says 13, only 12 present
                          .ok());
    TEST_ASSERT_FALSE(
        applyConfigWrite(settings, specs, bw::ble::kMaxConfigSettings, nullptr, len).ok());
    TEST_ASSERT_EQUAL_UINT32(0, settings.setCalls());
}
