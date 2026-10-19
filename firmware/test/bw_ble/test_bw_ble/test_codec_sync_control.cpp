// test_codec_sync_control.cpp - spec 9 row 5, RE-SCOPED by the G1 ruling on
// CR-019 (dumb byte pipe): F19 no longer parses Request/Ack - protocol
// semantics belong to F20. What F19 still owns and this suite pins: the
// Sync Control WRITE length bound and the verbatim byte passthrough into the
// inbound queue (unknown opcodes and versions flow through untouched, they
// are F20's to reject).

#include <cstring>

#include <unity.h>

#include <bw/ble/ble_config.h>
#include <bw/ble/codec.h>
#include <bw/ble/sync_frame_queue.h>

using bw::SyncChannel;
using bw::ble::checkSyncControlWrite;
using bw::ble::kGattContractVersion;
using bw::ble::kMaxSyncControlWriteBytes;
using bw::ble::SyncFrameQueue;

void test_sync_control_length_bounds(void) {
    uint8_t frame[kMaxSyncControlWriteBytes + 1] = {};
    // v1 layouts: Ack = 6 B, Request = 8 B - both inside the bound.
    TEST_ASSERT_TRUE(checkSyncControlWrite(frame, 6).ok());
    TEST_ASSERT_TRUE(checkSyncControlWrite(frame, 8).ok());
    TEST_ASSERT_TRUE(checkSyncControlWrite(frame, kMaxSyncControlWriteBytes).ok());
    // Zero and oversize are refused -> ATT 0x0D (contract 3).
    TEST_ASSERT_FALSE(checkSyncControlWrite(frame, 0).ok());
    TEST_ASSERT_FALSE(checkSyncControlWrite(frame, kMaxSyncControlWriteBytes + 1).ok());
}

void test_sync_control_bytes_stay_opaque(void) {
    // An UNKNOWN version byte and an UNKNOWN opcode: F19 must not parse or
    // reject them (CR-019 ruling 1 - F20 owns protocol errors).
    const uint8_t frame[8] = {0xEE, 0x42, 1, 2, 3, 4, 5, 6};
    TEST_ASSERT_TRUE(checkSyncControlWrite(frame, sizeof(frame)).ok());
    (void)kGattContractVersion;  // version checking is F20's, not F19's
}

void test_sync_control_passthrough_round_trip(void) {
    // The full device path minus NimBLE: callback check -> inbound queue ->
    // ISyncTransport::receive, byte-for-byte identical.
    SyncFrameQueue queue;
    const uint8_t frame[8] = {0xEE, 0x42, 1, 2, 3, 4, 5, 6};
    TEST_ASSERT_TRUE(checkSyncControlWrite(frame, sizeof(frame)).ok());
    TEST_ASSERT_TRUE(queue.push(SyncChannel::Control, frame, sizeof(frame)) == bw::Err::Ok);

    SyncChannel ch = SyncChannel::Data;
    uint8_t out[kMaxSyncControlWriteBytes] = {};
    const bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(sizeof(frame), got.value);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncChannel::Control), static_cast<int>(ch));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, sizeof(frame));
}
