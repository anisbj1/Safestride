#include <bw/ble/sync_transport.h>

namespace bw::ble {

Result<void> SyncTransport::send(SyncChannel ch, const uint8_t* data, uint16_t len) {
    if (data == nullptr || len == 0 || len > kMaxSyncFrameBytes) {
        return {Err::InvalidArg};  // caller bug; nothing sent (CR-019)
    }
    if (!linkUp()) {
        return {Err::NotReady};  // nothing queued across a down link (spec 10)
    }
    return deliver(ch, data, len);
}

Result<void> SyncTransport::deliver(SyncChannel ch, const uint8_t* data, uint16_t len) {
#if defined(ARDUINO)
    NimBLECharacteristic* target = nullptr;
    bool sent = false;
    if (ch == SyncChannel::Control) {
        target = chars_ != nullptr ? chars_->syncControl() : nullptr;
        if (target != nullptr) {
            // M4 bench finding #2: setValue()+indicate() routes through
            // ble_gatts_chr_updated(), which only ever delivers
            // notifications; an indication must use the explicit
            // value+length overload (ble_gattc_indicate_custom —
            // NimBLE-Arduino 2.5.1 NimBLECharacteristic.cpp:226-319
            // [SOURCE-VERIFIED] + bench proof, INTEGRATION_REPORT 11.3).
            // Also restores the CR-019 Busy contract: the old form always
            // returned "sent" while dropping the indication.
            sent = target->indicate(data, len);
        }
    } else {
        target = chars_ != nullptr ? chars_->syncData() : nullptr;
        if (target != nullptr) {
            target->setValue(data, len);
            sent = target->notify();
        }
    }
    return sent ? Result<void>{Err::Ok} : Result<void>{Err::Busy};
#else
    (void)ch;
    (void)data;
    (void)len;
    // No radio on the host (D12): tests override deliver() to record
    // attempts; an un-overridden base is an Unsupported hand-off, never a
    // fabricated success ("unknown/insufficient" is first-class, 00).
    return {Err::Unsupported};
#endif
}

Result<size_t> SyncTransport::receive(SyncChannel* ch_out, uint8_t* out, size_t cap) {
    return inbound_.receive(ch_out, out, cap);
}

bool SyncTransport::linkUp() const {
    return link_ != nullptr && link_->linkUsable();
}

uint16_t SyncTransport::maxSendBytes() const {
    return link_ != nullptr ? link_->maxSendBytes() : 0;
}

}  // namespace bw::ble
