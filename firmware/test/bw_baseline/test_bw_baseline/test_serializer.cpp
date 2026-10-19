// F13 §9 tests 10, 11, 12 — the 32 B blob codec: round trip, corruption /
// version rejection (including the tracker's restore() refusal path, §10) and
// the truncated / oversized edges with their CR-004 error split.

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_serializer.h>
#include <bw/baseline/baseline_tracker.h>

#include <bw/testkit/mem_kv_store.h>

#include <cstring>
#include <limits>

#include "fixtures.h"

namespace bl = bw::baseline;

namespace {

// A representative learned state with values that are not round in decimal, so
// the round trip proves the f32 bits survive (§9 test 10).
bl::TrackerState sampleState() {
    bl::TrackerState s{};
    s.hr_ewma = 61.75f;
    s.hr_absdev_ewma = 3.125f;
    s.enmo_ewma = 0.04375f;
    s.accepted_samples = 4321;
    return s;
}

}  // namespace

// §9.10 — state → bytes → state, bit-exact (16 B, no padding).
void test_serializer_round_trip(void) {
    const bl::TrackerState in = sampleState();
    uint8_t buf[64];
    const bw::Result<size_t> enc = bl::BaselineSerializer::serialize(in, buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.ok());
    TEST_ASSERT_EQUAL_UINT32(bl::kBlobBytes, enc.value);  // exactly 32, never more

    bl::TrackerState out{};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::deserialize(buf, enc.value, out).ok());
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
}

// §9.11 — flipped bits in magic / payload / CRC ⇒ Corrupt, a foreign schema ⇒
// Unsupported, and a tracker restore() refusal ⇒ discard + restart learning
// (§10). The store path surfaces the same Corrupt to the wiring fault hook.
void test_serializer_rejects_corrupt(void) {
    const bl::TrackerState in = sampleState();
    uint8_t good[64];
    TEST_ASSERT_TRUE(bl::BaselineSerializer::serialize(in, good, sizeof(good)).ok());

    uint8_t buf[64];
    bl::TrackerState out{};

    std::memcpy(buf, good, bl::kBlobBytes);
    buf[0] ^= 0x01u;  // magic byte
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Corrupt),
        static_cast<int>(bl::BaselineSerializer::deserialize(buf, bl::kBlobBytes, out).err));

    std::memcpy(buf, good, bl::kBlobBytes);
    buf[8] ^= 0x80u;  // hr_ewma payload byte — CRC must catch it
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Corrupt),
        static_cast<int>(bl::BaselineSerializer::deserialize(buf, bl::kBlobBytes, out).err));

    std::memcpy(buf, good, bl::kBlobBytes);
    buf[31] ^= 0x01u;  // CRC field itself
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Corrupt),
        static_cast<int>(bl::BaselineSerializer::deserialize(buf, bl::kBlobBytes, out).err));

    std::memcpy(buf, good, bl::kBlobBytes);
    buf[4] = static_cast<uint8_t>(bl::kBlobSchemaV1 + 1);  // well-formed future header
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Unsupported),
        static_cast<int>(bl::BaselineSerializer::deserialize(buf, bl::kBlobBytes, out).err));

    // Tracker refuses unusable states and keeps learning from scratch (§10).
    bl::BaselineTracker t;
    bl::TrackerState bad{};
    bad.hr_ewma = std::numeric_limits<float>::quiet_NaN();
    bad.hr_absdev_ewma = 2.0f;
    bad.enmo_ewma = 0.05f;
    bad.accepted_samples = 5000;
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Corrupt), static_cast<int>(t.restore(bad).err));

    bad = bl::TrackerState{};
    bad.hr_ewma = 999.0f;  // outside the plausibility band — fabricated HR if adopted
    bad.enmo_ewma = 0.05f;
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Corrupt), static_cast<int>(t.restore(bad).err));

    bad = bl::TrackerState{};
    bad.hr_ewma = 60.0f;
    bad.hr_absdev_ewma = -1.0f;  // a deviation cannot be negative
    bad.enmo_ewma = 0.05f;
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Corrupt), static_cast<int>(t.restore(bad).err));

    TEST_ASSERT_EQUAL_UINT32(0, t.state().accepted_samples);  // nothing was adopted
    bw::BaselineProfile p{};
    TEST_ASSERT_FALSE(t.profile(p));
    TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);  // discard ⇒ restart learning

    // Store path: a corrupt blob under kStoreKey surfaces Err::Corrupt for the
    // wiring to turn into FaultCode::BaselineCorrupt (D6, CR-031).
    bw::testkit::InMemoryKeyValueStore<> store;
    uint8_t garbage[bl::kBlobBytes];
    std::memset(garbage, 0xDE, sizeof(garbage));
    TEST_ASSERT_TRUE(store.put(bl::kStoreKey, garbage, sizeof(garbage)).ok());
    bl::TrackerState loaded{};
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Corrupt),
                      static_cast<int>(bl::BaselineSerializer::loadFromStore(&store, loaded).err));
}

// §9.12 — short input ⇒ Corrupt; serialize cap < kBlobBytes ⇒ Full with the
// buffer untouched; oversized reads stay strictly rejected.
void test_serializer_truncated_or_oversized(void) {
    const bl::TrackerState in = sampleState();
    uint8_t buf[64];
    std::memset(buf, 0xAA, sizeof(buf));

    const bw::Result<size_t> refused =
        bl::BaselineSerializer::serialize(in, buf, bl::kBlobBytes - 1);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Full), static_cast<int>(refused.err));
    for (size_t i = 0; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xAA, buf[i]);  // CR-004: nothing was written
    }

    uint8_t big[80];
    const bw::Result<size_t> enc = bl::BaselineSerializer::serialize(in, big, sizeof(big));
    TEST_ASSERT_TRUE(enc.ok());
    TEST_ASSERT_EQUAL_UINT32(bl::kBlobBytes, enc.value);  // exactly the layout, never more

    uint8_t good[64];
    TEST_ASSERT_TRUE(bl::BaselineSerializer::serialize(in, good, sizeof(good)).ok());

    bl::TrackerState out{};
    out.hr_ewma = 12345.0f;  // sentinel: an error must leave `out` untouched
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Corrupt),
        static_cast<int>(bl::BaselineSerializer::deserialize(good, bl::kBlobBytes - 1, out).err));
    TEST_ASSERT_EQUAL_FLOAT(12345.0f, out.hr_ewma);

    uint8_t oversized[40];
    std::memcpy(oversized, good, bl::kBlobBytes);
    std::memset(oversized + bl::kBlobBytes, 0x55, sizeof(oversized) - bl::kBlobBytes);
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Corrupt),
        static_cast<int>(
            bl::BaselineSerializer::deserialize(oversized, sizeof(oversized), out).err));
    TEST_ASSERT_EQUAL_FLOAT(12345.0f, out.hr_ewma);
}
