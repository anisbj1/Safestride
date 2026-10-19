// F28 §9.7 — the 48-byte Event binary vector (DATASET_FORMAT.md §5), C++ side.
//
// The same bytes are asserted by tools/capture/tests/test_decode_events.py: if
// bw::Event's layout ever drifts (CR-002: offsets are load-bearing), BOTH the
// firmware decoder contract and the Python decoder fail. The firmware never
// serialises Event — it is memcpy'd as-is, little-endian, no padding — so
// this test compares a memory image, not a serialisation.

#include <cstring>

#include <bw/contract_checks.h>
#include <bw/events.h>

#include <unity.h>

namespace {

// Test vector from DATASET_FORMAT.md §5 — byte-for-byte identical to
// VECTOR_HEX in test_decode_events.py.
const char* const kVectorHex = "15cd5b0700000000"
                               "03000000"
                               "07000000"
                               "88130000"
                               "00f15365"
                               "0000003f"
                               "04100000"
                               "deadbeef00010203"
                               "0002030104020501";

bw::Event makeVectorEvent() {
    bw::Event event{};
    event.t_start_ms = 123456789;
    event.seq = 3;
    event.boot_id = 7;
    event.duration_ms = 5000;
    event.t_start_unix = 1700000000;
    event.confidence = 0.5f;
    event.reasons = 4100;  // bits 2 (HrBelowBaseline) + 12 (CR-008)
    const uint8_t payload[bw::Event::kMaxPayloadBytes] = {0xDE, 0xAD, 0xBE, 0xEF,
                                                          0x00, 0x01, 0x02, 0x03};
    for (size_t i = 0; i < bw::Event::kMaxPayloadBytes; ++i) {
        event.payload[i] = payload[i];
    }
    event.quality = {bw::DataQuality::Good, bw::DataQuality::Insufficient};
    event.type = bw::EventType::UserMarked;
    event.label = bw::UserLabel::Confirmed;
    event.context = bw::ActivityContext::Walking;
    event.alertness_at_start = bw::AlertnessState::Awake;
    event.detector_id = 5;
    event.schema_version = bw::Event::kSchemaVersion;
    return event;
}

void hexEncode(const uint8_t* bytes, size_t len, char* out) {
    static const char* kDigits = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

}  // namespace

void test_event_size_is_pinned(void) {
    // The layout contract (F00 §8, CR-002): exactly 48 bytes, no padding. The
    // per-field offsets are static_asserted by bw/contract_checks.h on every
    // build; this test re-states the top-level size for the suite log.
    static_assert(sizeof(bw::Event) == bw::kEventBytes, "Event must be 48 B (CR-002)");
    TEST_ASSERT_EQUAL_UINT(bw::kEventBytes, sizeof(bw::Event));
}

void test_event_memory_image_matches_vector(void) {
    const bw::Event event = makeVectorEvent();
    uint8_t image[bw::kEventBytes];
    std::memcpy(image, &event, sizeof(event));

    char hex[bw::kEventBytes * 2 + 1];
    hexEncode(image, sizeof(image), hex);
    TEST_ASSERT_EQUAL_STRING(kVectorHex, hex);
}
