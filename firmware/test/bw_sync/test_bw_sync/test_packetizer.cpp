// bw_sync — packetizer (F20 §9 T16–T22, contract pins, golden vectors V2–V4).
// Byte-exact against SYNC_PROTOCOL.md §3/§6: a layout drift here is a failed
// contract, not a runtime surprise (D8).

#include <unity.h>

#include <cstring>

#include <bw/sync/chunk_packetizer.h>
#include <bw/sync/sync_config.h>
#include <bw/sync_transport.h>

#include "support/sync_fixtures.h"

using bw::sync::CursorRequest;
using bw::sync::decodeChunkFrame;
using bw::sync::encodeChunkFrame;
using bw::sync::encodeCursorFrame;
using bw::sync::encodeStatusFrame;
using bw::sync::kChunkFramingBytes;
using bw::sync::kCursorFrameBytes;
using bw::sync::kEventRecordBytes;
using bw::sync::kMaxEventsPerChunk;
using bw::sync::kMaxEventsPerRead;
using bw::sync::kStatusFrameBytes;
using bw::sync::parseCursorFrame;
using bw::sync::recordsPerChunk;

// -- T16/T17: round trip and CRC rejection ------------------------------------

void test_packetizer_chunk_round_trip(void) {
    bw::Event source[kMaxEventsPerRead];
    for (size_t i = 0; i < kMaxEventsPerRead; ++i) {
        source[i] = f20test::makeEvent(bw::EventType::DrowsinessEvent, 1000 + i);
        source[i].seq = static_cast<uint32_t>(i + 1);
    }
    uint8_t frame[bw::kMaxSyncFrameBytes];
    bw::Result<size_t> enc = encodeChunkFrame(7, source, 3, frame, sizeof frame);
    TEST_ASSERT_TRUE(enc.ok());
    TEST_ASSERT_EQUAL_UINT(3 * kEventRecordBytes + kChunkFramingBytes, enc.value);

    bw::Event decoded[kMaxEventsPerRead];
    uint16_t chunk_no = 0;
    bw::Result<size_t> dec =
        decodeChunkFrame(frame, enc.value, &chunk_no, decoded, kMaxEventsPerRead);
    TEST_ASSERT_TRUE(dec.ok());
    TEST_ASSERT_EQUAL_UINT16(7, chunk_no);
    TEST_ASSERT_EQUAL_UINT(3, dec.value);
    TEST_ASSERT_EQUAL_MEMORY(source, decoded, 3 * kEventRecordBytes);
}

void test_packetizer_corrupted_crc_rejected(void) {
    bw::Event source[1] = {f20test::makeEvent(bw::EventType::UserMarked, 5)};
    source[0].seq = 1;
    uint8_t frame[bw::kMaxSyncFrameBytes];
    bw::Result<size_t> enc = encodeChunkFrame(1, source, 1, frame, sizeof frame);
    TEST_ASSERT_TRUE(enc.ok());

    frame[enc.value - 1] ^= 0xFF;  // flip the CRC's high byte
    bw::Event decoded[kMaxEventsPerRead];
    uint16_t chunk_no = 0;
    bw::Result<size_t> dec =
        decodeChunkFrame(frame, enc.value, &chunk_no, decoded, kMaxEventsPerRead);
    TEST_ASSERT_FALSE(dec.ok());
    TEST_ASSERT_EQUAL(bw::Err::Corrupt, dec.err);
}

void test_packetizer_corrupted_payload_rejected(void) {
    bw::Event source[1] = {f20test::makeEvent(bw::EventType::UserMarked, 5)};
    source[0].seq = 1;
    uint8_t frame[bw::kMaxSyncFrameBytes];
    bw::Result<size_t> enc = encodeChunkFrame(1, source, 1, frame, sizeof frame);
    TEST_ASSERT_TRUE(enc.ok());

    frame[4] ^= 0x01;  // one payload bit: CRC must catch it
    bw::Event decoded[kMaxEventsPerRead];
    uint16_t chunk_no = 0;
    TEST_ASSERT_EQUAL(
        bw::Err::Corrupt,
        decodeChunkFrame(frame, enc.value, &chunk_no, decoded, kMaxEventsPerRead).err);
}

// -- T18/T19/T20: MTU boundaries and the whole-record property ----------------

void test_packetizer_mtu_floor_is_exactly_one_event(void) {
    // MTU 55 -> 52 usable -> exactly 1 whole record; 51 usable -> 0 (below floor).
    TEST_ASSERT_EQUAL_UINT(1, recordsPerChunk(bw::sync::kMinSyncUsableBytes));
    TEST_ASSERT_EQUAL_UINT(1, recordsPerChunk(51 + 1));
    TEST_ASSERT_EQUAL_UINT(0, recordsPerChunk(51));
    TEST_ASSERT_EQUAL_UINT(0, recordsPerChunk(0));
}

void test_packetizer_mtu_255_is_five_events(void) {
    TEST_ASSERT_EQUAL_UINT(kMaxEventsPerChunk, recordsPerChunk(bw::kMaxSyncFrameBytes));
    TEST_ASSERT_EQUAL_UINT(5, kMaxEventsPerChunk);
    TEST_ASSERT_EQUAL_UINT(5, recordsPerChunk(255 - 3));
    // Even a "larger than cap" usable never packs past the cap (buffer bound).
    TEST_ASSERT_EQUAL_UINT(5, recordsPerChunk(512));
}

void test_packetizer_payload_always_whole_records(void) {
    // Property (T20): every legal chunk length satisfies len % 48 == 4, for
    // every usable-byte count across the whole valid range.
    bw::Event source[kMaxEventsPerRead];
    for (size_t i = 0; i < kMaxEventsPerRead; ++i) {
        source[i].seq = static_cast<uint32_t>(i + 1);
    }
    for (uint16_t usable = bw::sync::kMinSyncUsableBytes; usable <= 255; ++usable) {
        const size_t records = recordsPerChunk(usable);
        TEST_ASSERT_TRUE(records >= 1 && records <= kMaxEventsPerChunk);
        uint8_t frame[bw::kMaxSyncFrameBytes];
        bw::Result<size_t> enc = encodeChunkFrame(1, source, records, frame, sizeof frame);
        TEST_ASSERT_TRUE(enc.ok());
        TEST_ASSERT_EQUAL_UINT(0, (enc.value - kChunkFramingBytes) % kEventRecordBytes);
        // A chunk may only under-fill the usable bytes (the 8 B slack at
        // usable = 248..251), never overflow them.
        TEST_ASSERT_TRUE(enc.value <= usable);
    }
}

// -- T21/T22: malformed input is dropped, never crashes -----------------------

void test_packetizer_unknown_version_rejected(void) {
    CursorRequest request;
    request.from_seq = 3;
    uint8_t frame[kCursorFrameBytes];
    encodeCursorFrame(request, frame);
    frame[0] = 99;  // unknown version
    CursorRequest out;
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg, parseCursorFrame(frame, sizeof frame, &out).err);
}

void test_packetizer_malformed_control_frames_rejected(void) {
    CursorRequest request;
    uint8_t frame[kCursorFrameBytes];
    encodeCursorFrame(request, frame);
    CursorRequest out;

    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      parseCursorFrame(frame, sizeof frame - 1, &out).err);  // short
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      parseCursorFrame(frame, sizeof frame + 1, &out).err);  // long
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg, parseCursorFrame(nullptr, 12, &out).err);
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg, parseCursorFrame(frame, 12, nullptr).err);

    uint8_t wrong_type[kCursorFrameBytes];
    std::memcpy(wrong_type, frame, sizeof frame);
    wrong_type[1] = 0x77;  // unknown type
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      parseCursorFrame(wrong_type, sizeof wrong_type, &out).err);
}

void test_packetizer_malformed_chunk_rejected(void) {
    bw::Event decoded[kMaxEventsPerRead];
    uint16_t chunk_no = 0;
    // 51 B: below one whole record; 53 B: not len % 48 == 4.
    uint8_t junk[64] = {};
    TEST_ASSERT_FALSE(decodeChunkFrame(junk, 51, &chunk_no, decoded, kMaxEventsPerRead).ok());
    TEST_ASSERT_FALSE(decodeChunkFrame(junk, 53, &chunk_no, decoded, kMaxEventsPerRead).ok());
    TEST_ASSERT_FALSE(decodeChunkFrame(nullptr, 52, &chunk_no, decoded, kMaxEventsPerRead).ok());
    TEST_ASSERT_FALSE(decodeChunkFrame(junk, 52, nullptr, decoded, kMaxEventsPerRead).ok());
}

void test_packetizer_encode_argument_guards(void) {
    bw::Event source[kMaxEventsPerChunk];
    uint8_t frame[bw::kMaxSyncFrameBytes];
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      encodeChunkFrame(1, source, 0, frame, sizeof frame).err);  // count == 0
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      encodeChunkFrame(1, source, kMaxEventsPerChunk + 1, frame, sizeof frame)
                          .err);  // count > cap
    TEST_ASSERT_EQUAL(bw::Err::InvalidArg,
                      encodeChunkFrame(1, source, 1, frame, 51).err);  // cap too small
    // Decoding a frame whose records exceed the caller buffer: Overflow, no write.
    bw::Event one[1] = {f20test::makeEvent(bw::EventType::Boot, 1)};
    bw::Result<size_t> enc = encodeChunkFrame(1, one, 1, frame, sizeof frame);
    TEST_ASSERT_TRUE(enc.ok());
    bw::Event tiny[1];
    uint16_t chunk_no = 0;
    TEST_ASSERT_EQUAL(bw::Err::Overflow,
                      decodeChunkFrame(frame, enc.value, &chunk_no, tiny, 0).err);
}

// -- contract pins (CR-019 test plan) ------------------------------------------

void test_packetizer_contract_size_pins(void) {
    TEST_ASSERT_EQUAL_UINT(bw::sync::kMaxSyncMtuBytes - 3, bw::kMaxSyncFrameBytes);
    TEST_ASSERT_EQUAL_UINT(12, kCursorFrameBytes);
    TEST_ASSERT_EQUAL_UINT(11, kStatusFrameBytes);
    TEST_ASSERT_EQUAL_UINT(52, bw::sync::kMinSyncUsableBytes);
    TEST_ASSERT_EQUAL_UINT(55, bw::sync::kMinSyncMtuBytes);
    TEST_ASSERT_EQUAL_UINT(4, kChunkFramingBytes);
    TEST_ASSERT_TRUE(sizeof(bw::Event) == kEventRecordBytes);
}

// -- golden vectors (SYNC_PROTOCOL.md §6 V2–V4) -------------------------------

void test_packetizer_v2_cursor_golden(void) {
    // V2: version=1, type=1, from_seq=1, max=64, ack_seq=0.
    CursorRequest request;
    request.from_seq = 1;
    request.max = 64;
    request.ack_seq = 0;
    uint8_t frame[kCursorFrameBytes];
    encodeCursorFrame(request, frame);
    const uint8_t expected[kCursorFrameBytes] = {0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
                                                 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, kCursorFrameBytes);

    CursorRequest back;
    TEST_ASSERT_TRUE(parseCursorFrame(frame, sizeof frame, &back).ok());
    TEST_ASSERT_EQUAL_UINT32(1, back.from_seq);
    TEST_ASSERT_EQUAL_UINT16(64, back.max);
    TEST_ASSERT_EQUAL_UINT32(0, back.ack_seq);
    TEST_ASSERT_FALSE(back.isStatusQuery());
}

void test_packetizer_v3_status_golden(void) {
    // V3: version=1, type=1, last_seq=100, last_synced=32, flags=0x01.
    uint8_t frame[kStatusFrameBytes];
    encodeStatusFrame(100, 32, 0x01, frame);
    const uint8_t expected[kStatusFrameBytes] = {0x01, 0x01, 0x64, 0x00, 0x00, 0x00,
                                                 0x20, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, kStatusFrameBytes);
}

void test_packetizer_v4_chunk_golden(void) {
    // V4: chunk_no=1 + the §6 fixture event + CRC-16/CCITT-FALSE (52 B frame).
    bw::Event fixture = f20test::makeEvent(bw::EventType::DrowsinessEvent, 1000);
    fixture.seq = 7;
    fixture.boot_id = 3;
    fixture.duration_ms = 250;
    fixture.confidence = 0.5f;
    fixture.detector_id = 9;
    uint8_t frame[bw::kMaxSyncFrameBytes];
    bw::Result<size_t> enc = encodeChunkFrame(1, &fixture, 1, frame, sizeof frame);
    TEST_ASSERT_TRUE(enc.ok());
    const uint8_t expected[52] = {0x01, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
                                  0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xFA, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x01, 0x00, 0x00, 0x00, 0x09, 0x01, 0xA9, 0xD2};
    TEST_ASSERT_EQUAL_UINT(52, enc.value);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, 52);
}
