// test_sync_frame_queue.cpp - the CR-019 inbound raw-frame queue (F19's D9
// pull model, internal after G1 ruling 1): FIFO round trip, channel tag,
// bounded length, Full on overflow (counted), Overflow on a too-small cap,
// and discard-on-disconnect via the generation stamp.

#include <bw/ble/sync_frame_queue.h>

#include <cstring>

#include <unity.h>

using bw::Err;
using bw::SyncChannel;
using bw::ble::kMaxSyncControlWriteBytes;
using bw::ble::SyncFrameQueue;

namespace {

const uint8_t kFrame[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

}  // namespace

void test_frame_queue_empty_receive(void) {
    SyncFrameQueue queue;
    SyncChannel ch = SyncChannel::Data;
    uint8_t out[kMaxSyncControlWriteBytes] = {};
    const bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(0, got.value);  // Ok(0) = nothing pending
}

void test_frame_queue_round_trip(void) {
    SyncFrameQueue queue;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));

    SyncChannel ch = SyncChannel::Data;
    uint8_t out[kMaxSyncControlWriteBytes] = {};
    const bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(8, got.value);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SyncChannel::Control), static_cast<int>(ch));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kFrame, out, 8);
}

void test_frame_queue_fifo_order(void) {
    SyncFrameQueue queue;
    const uint8_t second[2] = {0xAA, 0xBB};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, second, 2)));

    SyncChannel ch;
    uint8_t out[kMaxSyncControlWriteBytes] = {};
    bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(8, got.value);  // oldest first
    got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(2, got.value);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second, out, 2);
    got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(0, got.value);
}

void test_frame_queue_full_is_counted_refusal(void) {
    SyncFrameQueue queue;
    for (size_t i = 0; i < bw::ble::kSyncFrameQueueLen; ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                              static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));
    }
    // CR-004 Full: refused, nothing written, counter records the loss.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Full),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));
    TEST_ASSERT_EQUAL_UINT32(1, queue.drops());
}

void test_frame_queue_push_length_bounds(void) {
    SyncFrameQueue queue;
    uint8_t big[kMaxSyncControlWriteBytes + 1] = {};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg),
                          static_cast<int>(queue.push(SyncChannel::Control, big, sizeof(big))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg),
                          static_cast<int>(queue.push(SyncChannel::Control, nullptr, 4)));
    TEST_ASSERT_EQUAL_UINT(0, queue.pending());
}

void test_frame_queue_oversize_cap_drops_and_counts(void) {
    SyncFrameQueue queue;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));

    SyncChannel ch;
    uint8_t out[4] = {};  // too small for the 8 B frame
    const bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Overflow), static_cast<int>(got.err));
    TEST_ASSERT_EQUAL_UINT32(1, queue.oversizeDrops());
    TEST_ASSERT_EQUAL_UINT(0, queue.pending());  // the frame is gone, not stuck
}

void test_frame_queue_generation_discards_on_disconnect(void) {
    SyncFrameQueue queue;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));

    queue.bumpGeneration();  // link dropped: old bytes never leak (CR-019)

    SyncChannel ch;
    uint8_t out[kMaxSyncControlWriteBytes] = {};
    bw::Result<size_t> got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(0, got.value);  // Ok(0), exactly like an empty queue

    // A NEW link's frames arrive normally on the new generation.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(queue.push(SyncChannel::Control, kFrame, 8)));
    got = queue.receive(&ch, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(8, got.value);
}
