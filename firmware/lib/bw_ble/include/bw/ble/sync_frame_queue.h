#pragma once

// bw/ble/sync_frame_queue.h - F19 spec 7 (SyncFrameQueue row): the bounded
// phone->device raw-frame queue behind bw::ISyncTransport::receive() (CR-019
// ruling 1 keeps F19's D9 pull model, but with OPAQUE bytes - all protocol
// semantics live in F20's engine).
//
// Producer: the NimBLE host task (Sync Control write callback). Consumer:
// the F20 sync pump via SyncTransport::receive(). Thread-safe by SPSC
// construction; frames are discarded on disconnect through a generation
// stamp, so a dropped link never leaks stale bytes into the next session
// (ISyncTransport::receive: "Ok(0) ... right after a disconnect").

#include <atomic>
#include <cstdint>

#include <bw/ble/ble_config.h>
#include <bw/containers/spsc_queue.h>
#include <bw/result.h>
#include <bw/sync_transport.h>
#include <bw/types.h>

namespace bw::ble {

// One inbound raw frame. Only Sync Control writes reach this queue (Sync
// Data is device->phone only, BLE_GATT.md v1), so the payload slot is
// kMaxSyncControlWriteBytes - the ATT write bound F19 enforces.
struct RawFrame {
    uint8_t channel;  // bw::SyncChannel value
    uint8_t len;
    uint32_t generation;  // link generation the frame arrived on
    uint8_t data[kMaxSyncControlWriteBytes];
};

class SyncFrameQueue {
public:
    // Producer side (NimBLE callback or test). Err::Full = refused, nothing
    // written, drops() counts it (CR-004) - the callback never blocks.
    Err push(SyncChannel ch, const uint8_t* data, uint8_t len);

    // Consumer side, ISyncTransport::receive() semantics: copies the OLDEST
    // frame whose generation is still current; frames from a previous link
    // are discarded in passing. Ok(0) = nothing pending.
    //  Err::Overflow  the oldest current frame exceeded `cap`: it is dropped
    //                 and counted, caller retries with a bigger buffer.
    Result<size_t> receive(SyncChannel* ch_out, uint8_t* out, size_t cap);

    // Invalidates every pending frame: called when the link drops (the
    // producer and the disconnect signal both come from the NimBLE host
    // task, so no new frame can arrive on the old generation).
    void bumpGeneration();

    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    uint32_t drops() const { return queue_.drops(); }
    // Frames dropped because receive() was called with too small a cap
    // (consumer-owned counter, CR-004 Overflow sense).
    uint32_t oversizeDrops() const { return oversize_drops_; }
    size_t pending() const { return queue_.size(); }

private:
    SpscQueue<RawFrame, kSyncFrameQueueLen> queue_;
    std::atomic<uint32_t> generation_{1};
    uint32_t oversize_drops_ = 0;  // written by the consumer only
};

}  // namespace bw::ble
