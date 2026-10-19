// F18 §15.5 record-codec tests (T3 framing + D4 record round-trips).

#include <cstring>

#include <unity.h>

#include <bw/storage/record_codec.h>

using bw::storage::decodeRecord;
using bw::storage::encodeRecord;
using bw::storage::looksLikeRecordHeader;
using bw::storage::RecordType;

namespace {

void fillEvent(bw::Event& e) {
    std::memset(&e, 0, sizeof(e));
    e.t_start_ms = 4242;
    e.seq = 7;
    e.boot_id = 3;
    e.duration_ms = 1500;
    e.t_start_unix = 1700000000;
    e.confidence = 0.75f;
    e.reasons = 0x00000123;
    e.payload[0] = 0xAB;
    e.payload[7] = 0xCD;
    e.type = bw::EventType::DrowsinessEvent;
    e.label = bw::UserLabel::Confirmed;
    e.context = bw::ActivityContext::Stationary;
    e.alertness_at_start = bw::AlertnessState::Drowsy;
    e.detector_id = 2;
    e.schema_version = bw::Event::kSchemaVersion;
}

}  // namespace

void test_crc32_known_answer(void) {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, bw::storage::crc32(check, sizeof(check)));
}

void test_crc32_split_update_equals_single_pass(void) {
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    const uint32_t whole = bw::storage::crc32(data, sizeof(data));
    uint32_t split = bw::storage::crc32Update(0, data, 10);
    split = bw::storage::crc32Update(split, data + 10, 54);
    TEST_ASSERT_EQUAL_HEX32(whole, split);
}

void test_encode_decode_event_roundtrip(void) {
    bw::Event in{};
    fillEvent(in);
    uint8_t buf[bw::storage::kEventRecordBytes];
    const size_t n =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&in), sizeof(in), buf);
    TEST_ASSERT_EQUAL_UINT(bw::storage::kEventRecordBytes, n);

    auto dec = decodeRecord(buf, n);
    TEST_ASSERT_TRUE(dec.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RecordType::Event),
                            static_cast<uint8_t>(dec.value.type));
    TEST_ASSERT_EQUAL_UINT(n, dec.value.consumed);
    bw::Event out{};
    std::memcpy(&out, dec.value.payload, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(in.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT64(in.t_start_ms, out.t_start_ms);
    TEST_ASSERT_EQUAL_FLOAT(in.confidence, out.confidence);
    TEST_ASSERT_EQUAL_UINT8(in.schema_version, out.schema_version);
}

void test_decode_short_buffer_is_corrupt(void) {
    bw::Event in{};
    uint8_t buf[bw::storage::kEventRecordBytes];
    const size_t n =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&in), sizeof(in), buf);
    auto dec = decodeRecord(buf, n - 1);  // torn: last CRC byte missing
    TEST_ASSERT_FALSE(dec.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Corrupt), static_cast<uint8_t>(dec.err));
    TEST_ASSERT_EQUAL_UINT(0, dec.value.consumed);
}

void test_decode_bad_magic_is_corrupt(void) {
    bw::Event in{};
    uint8_t buf[bw::storage::kEventRecordBytes];
    const size_t n =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&in), sizeof(in), buf);
    buf[0] ^= 0xFF;
    TEST_ASSERT_FALSE(decodeRecord(buf, n).ok());
}

void test_decode_flipped_payload_byte_is_corrupt(void) {
    bw::Event in{};
    uint8_t buf[bw::storage::kEventRecordBytes];
    const size_t n =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&in), sizeof(in), buf);
    buf[bw::storage::kRecordHeaderBytes + 3] ^= 0x01;  // payload bit-flip
    TEST_ASSERT_FALSE(decodeRecord(buf, n).ok());
}

void test_encode_rejects_unknown_type_and_oversize(void) {
    uint8_t buf[bw::storage::kMaxRecordTotalBytes];
    const uint8_t payload[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT(0, encodeRecord(RecordType::None, payload, sizeof(payload), buf));
    uint8_t big[bw::storage::kMaxRecordPayloadBytes + 1] = {};
    TEST_ASSERT_EQUAL_UINT(0, encodeRecord(RecordType::Event, big, sizeof(big), buf));
}

void test_header_pre_filter_accepts_only_valid_headers(void) {
    bw::Event in{};
    uint8_t buf[bw::storage::kEventRecordBytes];
    const size_t n =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&in), sizeof(in), buf);
    RecordType type = RecordType::None;
    uint8_t len = 0;
    TEST_ASSERT_TRUE(looksLikeRecordHeader(buf, n, type, len));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RecordType::Event), static_cast<uint8_t>(type));
    TEST_ASSERT_EQUAL_UINT8(48, len);
    buf[4] = 99;  // unknown type
    TEST_ASSERT_FALSE(looksLikeRecordHeader(buf, n, type, len));
    uint8_t zeros[bw::storage::kRecordHeaderBytes] = {};
    TEST_ASSERT_FALSE(looksLikeRecordHeader(zeros, sizeof(zeros), type, len));
}

void test_label_patch_and_marker_roundtrip(void) {
    const uint8_t patch[5] = {0x10, 0x00, 0x00, 0x00, 2};  // seq 16, Rejected
    uint8_t buf[17];
    const size_t n = encodeRecord(RecordType::LabelPatch, patch, sizeof(patch), buf);
    TEST_ASSERT_EQUAL_UINT(bw::storage::kLabelPatchRecordBytes, n);
    auto dec = decodeRecord(buf, n);
    TEST_ASSERT_TRUE(dec.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RecordType::LabelPatch),
                            static_cast<uint8_t>(dec.value.type));
    TEST_ASSERT_EQUAL_UINT(5, dec.value.len);

    const uint8_t marker[4] = {0xFF, 0x00, 0x00, 0x00};  // seq 255
    uint8_t mbuf[16];
    const size_t mn = encodeRecord(RecordType::SyncMarker, marker, sizeof(marker), mbuf);
    TEST_ASSERT_EQUAL_UINT(bw::storage::kSyncMarkerRecordBytes, mn);
    auto mdec = decodeRecord(mbuf, mn);
    TEST_ASSERT_TRUE(mdec.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RecordType::SyncMarker),
                            static_cast<uint8_t>(mdec.value.type));
}
