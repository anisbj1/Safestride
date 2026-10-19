#pragma once

// bw_sync test support — LossyTransportFake: a private bw::ISyncTransport fake
// (F20 spec §7) with two inboxes, a link flag and DETERMINISTIC scripted
// faults only (no randomness): drop / duplicate / hold(reorder) the next Data
// frame OR the nth Data frame (absolute index), error-inject the next N sends
// on any channel (Busy etc.), link up/down by the test (setLinkUp — the fake
// has no scripted "disconnect at nth send"), MTU override (setUsableBytes).
// Implements the CR-019 contract semantics exactly — Ok(0) on an empty inbox,
// Overflow when a pending frame exceeds cap (head frame dropped), inbound
// queues discarded when the link drops, send() rejects len == 0 or
// len > kMaxSyncFrameBytes with InvalidArg.

#include <cstddef>
#include <cstdint>

#include <bw/sync_transport.h>
#include <bw/testkit/host_only.h>

namespace f20test {

class LossyTransportFake final : public bw::ISyncTransport {
public:
    // -- bw::ISyncTransport (device side) ------------------------------------
    bw::Result<void> send(bw::SyncChannel ch, const std::uint8_t* data, std::uint16_t len) override;
    bw::Result<std::size_t> receive(bw::SyncChannel* ch_out, std::uint8_t* out,
                                    std::size_t cap) override;
    bool linkUp() const override { return link_up_; }
    std::uint16_t maxSendBytes() const override { return link_up_ ? usable_bytes_ : 0; }

    // -- test controls --------------------------------------------------------
    // Link down discards BOTH inboxes (contract: device inbound queues on drop;
    // device->phone notifications are lost with the link — worst-case honest).
    void setLinkUp(bool up);
    // Usable Data-frame bytes while up (ATT MTU - 3); default = MTU 255 - 3.
    void setUsableBytes(std::uint16_t usable_bytes);
    // Phone -> device frame (the harness writes CURSOR here). False if queue full.
    bool pushToDevice(bw::SyncChannel ch, const std::uint8_t* data, std::size_t len);
    // Scripted faults, consumed on use:
    void failNextSend(bw::Err err, std::size_t times = 1);  // send() returns err
    void dropNextData();                  // next Data send() returns Ok but frame vanishes (F01)
    void dropDataFrame(std::size_t nth);  // the nth Data send overall vanishes (matrix)
    void duplicateNextData();             // next Data frame is delivered twice (F02)
    void duplicateDataFrame(std::size_t nth);  // the nth Data frame is delivered twice (matrix)
    void holdNextData();                  // hold next Data frame; deliver after the one after (F03)
    void holdDataFrame(std::size_t nth);  // same reorder, armed at the nth Data send (matrix)

    // -- observations ---------------------------------------------------------
    std::size_t
    dataFramesAccepted() const;  // Data sends the transport took (incl. dropped-by-fault)
    std::size_t controlFramesAccepted() const;
    std::size_t droppedByFault() const;      // frames vanished by dropNextData()
    std::size_t reorderedByFault() const;    // held frames released out of order (F03/matrix)
    std::size_t phoneFramesPending() const;  // device->phone frames the harness has not taken
    // Oldest device->phone frame; returns bytes copied, 0 = none pending.
    std::size_t takePhoneFrame(bw::SyncChannel* ch, std::uint8_t* out, std::size_t cap);
    void reset();

private:
    struct Frame {
        bw::SyncChannel ch = bw::SyncChannel::Control;
        std::uint16_t len = 0;
        std::uint8_t data[bw::kMaxSyncFrameBytes]{};
    };

    bool enqueueDevice(Frame& frame);  // phone-inbox push (device->phone)

    // device -> phone (harness drains)
    Frame phone_queue_[32];
    std::size_t phone_head_ = 0;
    std::size_t phone_count_ = 0;
    // phone -> device (engine drains)
    Frame device_queue_[8];
    std::size_t device_head_ = 0;
    std::size_t device_count_ = 0;

    bool link_up_ = false;
    std::uint16_t usable_bytes_ = 252;  // MTU 255 - 3 (kMaxSyncFrameBytes)
    std::size_t fail_remaining_ = 0;
    bw::Err fail_err_ = bw::Err::Ok;
    bool drop_next_data_ = false;
    std::size_t drop_data_at_ = 0;  // absolute Data-send index to drop (0 = none)
    bool duplicate_next_data_ = false;
    std::size_t duplicate_data_at_ = 0;  // absolute Data-send index to duplicate (0 = none)
    bool hold_next_data_ = false;
    std::size_t hold_data_at_ = 0;  // absolute Data-send index to hold/reorder (0 = none)
    Frame held_;
    bool holding_ = false;

    std::size_t data_accepted_ = 0;
    std::size_t control_accepted_ = 0;
    std::size_t dropped_by_fault_ = 0;
    std::size_t reordered_by_fault_ = 0;
};

}  // namespace f20test
