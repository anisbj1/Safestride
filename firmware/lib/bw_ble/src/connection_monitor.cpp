#include <bw/ble/connection_monitor.h>

#if defined(ARDUINO)

#include <cstring>

#include <bw/ble/ble_config.h>
#include <bw/ble/codec.h>
// ble_gattc_exchange_mtu (CR-026) — full-path include, the same style
// gatt_database.cpp uses for nimble/nimble/host headers.
#include <nimble/nimble/host/include/host/ble_gatt.h>

namespace bw::ble {

namespace {

// [HW] NimBLE 2.5.1 onSubscribe subValue: 0 = CCCD cleared, 1 = notifications
// enabled, 2 = indications enabled (NimBLECharacteristic.h [MEASURED]). The
// 1.x BLE_UNSUBSCRIBE enum no longer exists; "any non-zero = subscribed" is
// the correct test for every CCCD row F19 uses.
constexpr uint16_t kSubscribedNone = 0;

}  // namespace

ConnectionMonitor::ConnectionMonitor() : provider_(&state_mailbox_) {}

void ConnectionMonitor::setup(SyncFrameQueue* inbound, const IClock* clock) {
    inbound_ = inbound;
    clock_ = clock;
    // Static-storage FreeRTOS queue: created before any task runs, so no
    // heap after startup (00 7.2); every consumer wait has a timeout.
    actions_ = xQueueCreateStatic(kGattActionQueueLen, sizeof(BleAction), actions_storage_,
                                  &actions_meta_);
}

void ConnectionMonitor::setChars(const GattDatabase::Chars* chars) {
    chars_ = chars;
}

bool ConnectionMonitor::popAction(BleAction* out, uint32_t wait_ms) {
    if (actions_ == nullptr || out == nullptr) {
        return false;
    }
    return xQueueReceive(actions_, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

bool ConnectionMonitor::linkUsable() const {
    // CR-019 linkUp(): connected + encrypted per bonding policy + the peer
    // enabled Sync Data notifications.
    return connected_.load(std::memory_order_acquire) &&
           encrypted_.load(std::memory_order_acquire) &&
           sync_subscribed_.load(std::memory_order_acquire);
}

uint16_t ConnectionMonitor::maxSendBytes() const {
    if (!connected_.load(std::memory_order_acquire)) {
        return 0;  // no link: nothing usable (ISyncTransport contract)
    }
    const uint16_t mtu = mtu_.load(std::memory_order_acquire);
    if (mtu <= kAttHeaderBytes) {
        return 0;
    }
    const uint16_t usable = static_cast<uint16_t>(mtu - kAttHeaderBytes);
    return usable > kMaxSyncFrameBytes ? kMaxSyncFrameBytes : usable;
}

void ConnectionMonitor::applyLinkEvent(const LinkEvent& ev) {
    tracker_.apply(ev);  // tracker is callback-context only: no races
    LinkFlags flags{};
    if (tracker_.flags(flags)) {
        connected_.store(flags.connected, std::memory_order_release);
        encrypted_.store(flags.encrypted, std::memory_order_release);
        sync_subscribed_.store(flags.sync_data_subscribed, std::memory_order_release);
        mtu_.store(flags.mtu, std::memory_order_release);
    }
    publishState();
}

void ConnectionMonitor::publishState() {
    BleLinkState state{};
    if (tracker_.snapshot(state)) {
        state_mailbox_.set(state);  // CR-018: F23/F12 read through the provider
    }
}

bool ConnectionMonitor::pushAction(const BleAction& action) {
    if (actions_ == nullptr) {
        return false;
    }
    if (xQueueSend(actions_, &action, 0) != pdTRUE) {
        ++stats_.actions_dropped;  // never block the host task (spec 10)
        return false;
    }
    return true;
}

int ConnectionMonitor::rowOf(const NimBLECharacteristic* chr) const {
    if (chars_ == nullptr || chr == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < kGattCharacteristicCount; ++i) {
        if (chars_->by_row[i] == chr) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---- GAP / security --------------------------------------------------------

void ConnectionMonitor::onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) {
    (void)srv;
    // A fresh link starts unencrypted and un-subscribed (D1) at
    // kDefaultAttMtu; the negotiated value replaces it via onMTUChange.
    applyLinkEvent(
        {LinkEvent::Kind::Up, kDefaultAttMtu, false, clock_ != nullptr ? clock_->nowMs() : 0});
    // The DEVICE initiates the MTU exchange (CR-026): no tested central ever
    // does — Windows/bleak exposes no ATT_MTU_REQ API at all (measured link
    // stays at kDefaultAttMtu) and the W4 nRF Connect session sent none
    // either — while F20 streams only at the sync floor (SYNC_PROTOCOL.md
    // §3.3: ATT MTU >= 55), so waiting on the peer deadlocked the streaming
    // leg at kDefaultAttMtu on every PC-side run (queue 10). Fire-and-forget:
    // a NULL cb is contract-legal inside NimBLE (ble_gattc_mtu_cb
    // null-guards it) and the result arrives as BLE_GAP_EVENT_MTU →
    // onMTUChange below. The request carries the preferred MTU already
    // installed by NimBLEDevice::setMTU(config_.preferred_mtu) in
    // ble_server.cpp (ble_att_create_chan: chan->my_mtu = preferred). A
    // failed call leaves the link at kDefaultAttMtu, where the existing
    // floors keep everything fail-closed — never a fabricated pass.
    (void)ble_gattc_exchange_mtu(connInfo.getConnHandle(), nullptr, nullptr);
}

void ConnectionMonitor::onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) {
    (void)srv;
    (void)connInfo;
    (void)reason;
    // Nothing survives a dropped link: per-link state resets (tracker) and
    // queued inbound sync frames from the old link are invalidated, so
    // ISyncTransport::receive() reports "nothing pending" after a drop
    // (CR-019; spec 10 "F19 queues nothing across a disconnect").
    if (inbound_ != nullptr) {
        inbound_->bumpGeneration();
    }
    applyLinkEvent({LinkEvent::Kind::Down, 0, false, clock_ != nullptr ? clock_->nowMs() : 0});
}

void ConnectionMonitor::onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) {
    (void)connInfo;
    applyLinkEvent(
        {LinkEvent::Kind::MtuChanged, MTU, false, clock_ != nullptr ? clock_->nowMs() : 0});
}

void ConnectionMonitor::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    // "Encrypted" for CR-018 / linkUp() means the link is both encrypted and
    // bonded (D1: a reconnected phone must already share a bond).
    const bool secured = connInfo.isEncrypted() && connInfo.isBonded();
    applyLinkEvent(
        {LinkEvent::Kind::EncryptedChanged, 0, secured, clock_ != nullptr ? clock_->nowMs() : 0});
}

// ---- characteristic writes / CCCD -----------------------------------------

void ConnectionMonitor::onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) {
    (void)connInfo;
    const int row = rowOf(chr);
    if (row < 0) {
        return;
    }
    // NimBLE 2.5.1 [MEASURED] writeEvent() stores the value BEFORE this
    // callback and offers no write-reject path (the 1.x setWriteFlag /
    // setWriteErrorCode pair is gone - NimBLECharacteristic.cpp). So the
    // callback layer can only count refusals (spec D8/D6): over-long writes
    // never reach here (the stack rejects them against the characteristic's
    // max_len with ATT 0x0D at creation, gatt_database.cpp); what reaches
    // this switch is length-legal, and a semantic refusal is counted and
    // dropped without ever reaching a provider.
    const NimBLEAttValue& value = chr->getValue();
    const size_t len = value.size();

    BleAction action{};
    action.char_row = static_cast<uint8_t>(row);
    action.len = static_cast<uint8_t>(len > kMaxActionBytes ? kMaxActionBytes : len);
    if (action.len > 0) {
        std::memcpy(action.data, value.data(), action.len);
    }

    bool accepted = true;
    switch (static_cast<uint8_t>(row)) {
    case kRowCommand:
        action.kind = BleActionKind::WriteCommand;
        accepted = (len == kCommandBytes);  // pinned 9 B image (D6)
        break;
    case kRowConfig:
        action.kind = BleActionKind::WriteConfig;
        // Header minimum now, full name bound validated on the task
        // side where ISettings may be read (spec D8).
        accepted = (len >= 9 && len <= kMaxConfigWriteBytes);
        break;
    case kRowSyncControl:
        action.kind = BleActionKind::WriteSyncControl;
        // Length bound only: bytes stay opaque to F19 (CR-019 ruling 1).
        accepted = checkSyncControlWrite(value.data(), len).ok();
        break;
    default:
        return;  // no other characteristic accepts writes
    }

    if (!accepted) {
        // Counted + dropped, never an ATT error (see the note above): F19
        // stays honest about "the phone saw a success, the device refused".
        ++stats_.writes_rejected;
        return;
    }
    (void)pushAction(action);
}

void ConnectionMonitor::onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo,
                                    uint16_t subValue) {
    (void)connInfo;
    const int row = rowOf(chr);
    if (row < 0) {
        return;
    }
    const bool enabled = (subValue != kSubscribedNone);
    switch (static_cast<uint8_t>(row)) {
    case kRowSyncData:
        // Drives ISyncTransport::linkUp() and the CR-018 snapshot.
        applyLinkEvent({LinkEvent::Kind::SyncDataSubscribedChanged, 0, enabled,
                        clock_ != nullptr ? clock_->nowMs() : 0});
        break;
    case kRowEventNotify:
        // The notifier baselines on the CommsTask side (spec D7) - the
        // callback only reports the edge (bounded, no store reads here).
        (void)pushAction(
            {BleActionKind::Subscribe, kRowEventNotify, static_cast<uint8_t>(enabled), 0, {}});
        break;
    default:
        break;  // Live Status / HR / Battery: notify() no-ops when off
    }
}

}  // namespace bw::ble

#endif  // ARDUINO
