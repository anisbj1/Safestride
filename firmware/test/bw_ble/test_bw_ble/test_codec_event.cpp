// test_codec_event.cpp - spec 9 row 6: the 48-byte passthrough and the
// static_assert contract (sizeof / packing / kSchemaVersion) of the CANONICAL
// bw::Event image (data_decision.md 4.5 / CR-002; CR-019 ruling 3: no second
// event encoding may exist).

#include <bw/ble/codec.h>

#include <cstring>

#include <unity.h>

#include "ble_test_fakes.h"

using bw::Event;
using bw::ble::encodeEvent;
using bw::ble::kMaxEventNotifyBytes;

// The contract pin, in the build itself (spec 9).
static_assert(sizeof(Event) == 48, "Event must stay the canonical 48-byte record");
static_assert(Event::kSchemaVersion == 1, "Event schema version pinned by CR-002");

void test_event_passthrough_is_verbatim(void) {
    Event source = bw_ble_test::makeEvent(42, bw::EventType::DrowsinessEvent, 5'000);
    source.confidence = 0.75f;
    source.reasons = 0x0000'1000u;
    source.detector_id = 6;
    std::memcpy(source.payload, "12345678", 8);

    uint8_t buf[kMaxEventNotifyBytes] = {};
    const bw::Result<size_t> written = encodeEvent(source, buf, sizeof(buf));
    TEST_ASSERT_TRUE(written.ok());
    TEST_ASSERT_EQUAL_UINT(kMaxEventNotifyBytes, written.value);

    Event decoded{};
    std::memcpy(&decoded, buf, sizeof(Event));
    TEST_ASSERT_EQUAL_UINT64(source.t_start_ms, decoded.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(42, decoded.seq);
    TEST_ASSERT_EQUAL_UINT32(source.boot_id, decoded.boot_id);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, decoded.confidence);
    TEST_ASSERT_EQUAL_HEX32(0x1000u, decoded.reasons);
    TEST_ASSERT_EQUAL_UINT8(6, decoded.detector_id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EventType::DrowsinessEvent),
                            static_cast<uint8_t>(decoded.type));
    TEST_ASSERT_EQUAL_UINT8(1, decoded.schema_version);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(source.payload, decoded.payload, 8);
}

void test_event_encode_rejects_small_cap(void) {
    const Event source = bw_ble_test::makeEvent(1, bw::EventType::Boot, 0);
    uint8_t buf[kMaxEventNotifyBytes] = {};
    TEST_ASSERT_FALSE(encodeEvent(source, buf, kMaxEventNotifyBytes - 1).ok());
    TEST_ASSERT_FALSE(encodeEvent(source, nullptr, sizeof(buf)).ok());
}
