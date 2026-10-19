#include <bw/ble/sync_frame_queue.h>

#include <cstring>

namespace bw::ble {

Err SyncFrameQueue::push(SyncChannel ch, const uint8_t* data, uint8_t len) {
    if (data == nullptr || len == 0 || len > kMaxSyncControlWriteBytes) {
        return Err::InvalidArg;  // caller bug; nothing written
    }
    RawFrame frame{};
    frame.channel = static_cast<uint8_t>(ch);
    frame.len = len;
    frame.generation = generation_.load(std::memory_order_acquire);
    std::memcpy(frame.data, data, len);
    return queue_.push(frame);  // Err::Full on a full queue, drops() counts it
}

Result<size_t> SyncFrameQueue::receive(SyncChannel* ch_out, uint8_t* out, size_t cap) {
    if (ch_out == nullptr || out == nullptr) {
        return {Err::InvalidArg, 0};
    }
    const uint32_t current = generation_.load(std::memory_order_acquire);
    for (;;) {
        RawFrame frame{};
        if (!queue_.pop(frame)) {
            return {Err::Ok, 0};  // nothing pending (also right after a drop)
        }
        if (frame.generation != current) {
            continue;  // stale bytes from a previous link: discarded by design
        }
        if (frame.len > cap) {
            ++oversize_drops_;  // caller passed too small a buffer: counted
            return {Err::Overflow, 0};
        }
        *ch_out = static_cast<SyncChannel>(frame.channel);
        std::memcpy(out, frame.data, frame.len);
        return {Err::Ok, frame.len};
    }
}

void SyncFrameQueue::bumpGeneration() {
    // Any frame already queued belongs to the old link; receive() skips it.
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace bw::ble
