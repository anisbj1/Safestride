#include "lossy_transport_fake.h"

#include <cstring>

namespace f20test {

bw::Result<void> LossyTransportFake::send(bw::SyncChannel ch, const std::uint8_t* data,
                                          std::uint16_t len) {
    if (len == 0 || len > bw::kMaxSyncFrameBytes || data == nullptr) {
        return {bw::Err::InvalidArg};  // caller bug per contract: nothing consumed
    }
    if (!link_up_) {
        return {bw::Err::NotReady};
    }
    if (fail_remaining_ > 0) {
        --fail_remaining_;
        return {fail_err_};
    }
    Frame frame;
    frame.ch = ch;
    frame.len = len;
    std::memcpy(frame.data, data, len);

    if (ch == bw::SyncChannel::Data) {
        ++data_accepted_;
        if (drop_next_data_ || (drop_data_at_ != 0 && data_accepted_ == drop_data_at_)) {
            drop_next_data_ = false;
            drop_data_at_ = 0;
            ++dropped_by_fault_;  // accepted per contract, then lost on the air (F01)
            return {bw::Err::Ok};
        }
        if (duplicate_next_data_ ||
            (duplicate_data_at_ != 0 && data_accepted_ == duplicate_data_at_)) {
            duplicate_next_data_ = false;
            duplicate_data_at_ = 0;
            enqueueDevice(frame);  // second copy behind the first (F02)
        }
        if (hold_next_data_ || (hold_data_at_ != 0 && data_accepted_ == hold_data_at_)) {
            hold_next_data_ = false;
            hold_data_at_ = 0;
            held_ = frame;  // deliver after the NEXT data frame (F03 reorder)
            holding_ = true;
            return {bw::Err::Ok};
        }
        if (holding_) {
            enqueueDevice(frame);  // the held frame goes out after this one
            enqueueDevice(held_);
            holding_ = false;
            ++reordered_by_fault_;  // observable: a held frame was released late
            return {bw::Err::Ok};
        }
        enqueueDevice(frame);
        return {bw::Err::Ok};
    }

    ++control_accepted_;
    enqueueDevice(frame);
    return {bw::Err::Ok};
}

bw::Result<std::size_t> LossyTransportFake::receive(bw::SyncChannel* ch_out, std::uint8_t* out,
                                                    std::size_t cap) {
    if (ch_out == nullptr || out == nullptr) {
        return {bw::Err::InvalidArg, 0};
    }
    if (device_count_ == 0) {
        return {bw::Err::Ok, 0};
    }
    Frame& head = device_queue_[device_head_];
    if (head.len > cap) {
        // Contract: the oversized pending frame is DROPPED and Overflow reported.
        device_head_ = (device_head_ + 1) % 8;
        --device_count_;
        return {bw::Err::Overflow, 0};
    }
    *ch_out = head.ch;
    std::memcpy(out, head.data, head.len);
    const std::size_t len = head.len;
    device_head_ = (device_head_ + 1) % 8;
    --device_count_;
    return {bw::Err::Ok, len};
}

void LossyTransportFake::setLinkUp(bool up) {
    link_up_ = up;
    if (!up) {
        // Both directions die with the link: nothing survives a disconnect.
        phone_head_ = 0;
        phone_count_ = 0;
        device_head_ = 0;
        device_count_ = 0;
        holding_ = false;
    }
}

void LossyTransportFake::setUsableBytes(std::uint16_t usable_bytes) {
    usable_bytes_ = usable_bytes;
}

bool LossyTransportFake::pushToDevice(bw::SyncChannel ch, const std::uint8_t* data,
                                      std::size_t len) {
    if (data == nullptr || len == 0 || len > bw::kMaxSyncFrameBytes) {
        return false;
    }
    if (device_count_ >= 8) {
        return false;
    }
    Frame& slot = device_queue_[(device_head_ + device_count_) % 8];
    slot.ch = ch;
    slot.len = static_cast<std::uint16_t>(len);
    std::memcpy(slot.data, data, len);
    ++device_count_;
    return true;
}

void LossyTransportFake::failNextSend(bw::Err err, std::size_t times) {
    fail_err_ = err;
    fail_remaining_ = times;
}

void LossyTransportFake::dropNextData() {
    drop_next_data_ = true;
}

void LossyTransportFake::dropDataFrame(std::size_t nth) {
    drop_data_at_ = nth;
}

void LossyTransportFake::duplicateNextData() {
    duplicate_next_data_ = true;
}

void LossyTransportFake::duplicateDataFrame(std::size_t nth) {
    duplicate_data_at_ = nth;
}

void LossyTransportFake::holdNextData() {
    hold_next_data_ = true;
}

void LossyTransportFake::holdDataFrame(std::size_t nth) {
    hold_data_at_ = nth;
}

std::size_t LossyTransportFake::dataFramesAccepted() const {
    return data_accepted_;
}

std::size_t LossyTransportFake::controlFramesAccepted() const {
    return control_accepted_;
}

std::size_t LossyTransportFake::droppedByFault() const {
    return dropped_by_fault_;
}

std::size_t LossyTransportFake::reorderedByFault() const {
    return reordered_by_fault_;
}

std::size_t LossyTransportFake::phoneFramesPending() const {
    return phone_count_;
}

std::size_t LossyTransportFake::takePhoneFrame(bw::SyncChannel* ch, std::uint8_t* out,
                                               std::size_t cap) {
    if (phone_count_ == 0) {
        return 0;
    }
    Frame& head = phone_queue_[phone_head_];
    if (head.len > cap) {
        return 0;  // test-side misuse guard; the harness always passes full capacity
    }
    if (ch != nullptr) {
        *ch = head.ch;
    }
    std::memcpy(out, head.data, head.len);
    const std::size_t len = head.len;
    phone_head_ = (phone_head_ + 1) % 32;
    --phone_count_;
    return len;
}

void LossyTransportFake::reset() {
    phone_head_ = 0;
    phone_count_ = 0;
    device_head_ = 0;
    device_count_ = 0;
    link_up_ = false;
    usable_bytes_ = 252;
    fail_remaining_ = 0;
    drop_next_data_ = false;
    drop_data_at_ = 0;
    duplicate_next_data_ = false;
    duplicate_data_at_ = 0;
    hold_next_data_ = false;
    hold_data_at_ = 0;
    holding_ = false;
    data_accepted_ = 0;
    control_accepted_ = 0;
    dropped_by_fault_ = 0;
    reordered_by_fault_ = 0;
}

bool LossyTransportFake::enqueueDevice(Frame& frame) {
    if (phone_count_ >= 32) {
        return false;  // harness too slow in a test script: bounded, counted nothing
    }
    phone_queue_[(phone_head_ + phone_count_) % 32] = frame;
    ++phone_count_;
    return true;
}

}  // namespace f20test
