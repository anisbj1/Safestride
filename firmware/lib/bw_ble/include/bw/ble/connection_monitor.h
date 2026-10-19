#pragma once

// bw/ble/connection_monitor.h - F19 spec 7 (ConnectionMonitor row): the
// NimBLE gap/MTU/security/CCCD callbacks translated into LinkEvents
// (LinkStateTracker), the CR-018 BleLinkState mailbox, and bounded BleAction
// pushes for the CommsTask (spec D8: bounded work in the callback, everything
// downstream on the task side). Device-only.

#if defined(ARDUINO)

#include <NimBLEDevice.h>

#include <atomic>

#include <bw/ble/ble_config.h>
#include <bw/ble/gatt_database.h>
#include <bw/ble/link_state_tracker.h>
#include <bw/ble/sync_frame_queue.h>
#include <bw/ble/sync_link.h>
#include <bw/clock.h>
#include <bw/containers/latest_value.h>
#include <bw/types.h>

namespace bw::ble {

// D8: what the NimBLE host task hands the CommsTask. Writes carry the raw
// bytes (decode needs the monotonic clock for the Command t_ms stamp, which
// the host task must not reach for - spec D8 forbids provider reads in
// callback context; bounded O(1) either way).
enum class BleActionKind : uint8_t {
    WriteCommand,      // Command characteristic write (9 B, already length-checked)
    WriteConfig,       // Config characteristic write
    WriteSyncControl,  // Sync Control write -> raw inbound frame for F20
    Subscribe,         // CCCD toggle (char_row + subscribed flag)
};

struct BleAction {
    BleActionKind kind;
    uint8_t char_row;    // kRow* of the source characteristic
    uint8_t subscribed;  // Subscribe only: 1 = notifications enabled
    uint8_t len;
    uint8_t data[kMaxActionBytes];
};

// Diagnostics counters (spec 10; H-card evidence).
struct ConnectionStats {
    uint32_t actions_dropped;  // action queue full: writes lost, counted (never blocking)
    // NimBLE 2.5.1 [MEASURED] stores every in-bound-length write before the
    // onWrite callback (NimBLECharacteristic::writeEvent), so a write F19
    // refuses on length/type grounds is counted here, never an ATT error -
    // BLE_GATT.md v1 3 documents this per characteristic.
    uint32_t writes_rejected;
};

// The NimBLE gap/MTU/security/CCCD callbacks translated into LinkEvents,
// the CR-018 mailbox and bounded BleAction pushes (file header). Also the
// ISyncLink implementation (spec D12): SyncTransport observes the link
// through that pure seam, which makes the wiring bind host-testable
// (M4 bench bug 1).
class ConnectionMonitor : public NimBLEServerCallbacks,
                          public NimBLECharacteristicCallbacks,
                          public ISyncLink {
public:
    ConnectionMonitor();

    // Binds the inbound sync-frame queue (transport side), the monotonic
    // clock and creates the action queue in static storage (no heap).
    void setup(SyncFrameQueue* inbound, const IClock* clock);

    // Characteristic pointers, set once GattDatabase::create() has run.
    void setChars(const GattDatabase::Chars* chars);

    // CommsTask side. wait_ms = 0 polls; otherwise blocks at most that long
    // (every wait has a timeout - 00 7.2). false = nothing arrived.
    bool popAction(BleAction* out, uint32_t wait_ms);

    // Any-task reads for the transport (single-atomics, no locks).
    bool linkUsable() const override;  // connected + encrypted + peer subscribed Sync Data (CR-019)
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    // Encrypted-bond link flag: Feed to PublishGate::setLink so no notification
    // leaves the device over an unencrypted link (audit AUD-1 layer 2).
    bool encrypted() const { return encrypted_.load(std::memory_order_acquire); }
    // ATT MTU - 3, 0 while no link. [MEASURED] CR-019: the sync engine needs
    // 52 usable bytes, i.e. ATT MTU >= 55 (the 51-B figure is the separate
    // Event-notify floor, kMinEventNotifyMtu).
    uint16_t maxSendBytes() const override;

    // CR-018 output for F23/F12.
    LinkStateProvider& provider() { return provider_; }
    const ConnectionStats& stats() const { return stats_; }

    // ---- NimBLEServerCallbacks (NimBLE 2.5.1 [MEASURED] signatures) --------
    void onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) override;
    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

    // ---- NimBLECharacteristicCallbacks -------------------------------------
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override;
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo,
                     uint16_t subValue) override;

private:
    void applyLinkEvent(const LinkEvent& ev);
    void publishState();
    bool pushAction(const BleAction& action);
    int rowOf(const NimBLECharacteristic* chr) const;  // -1 = not ours

    SyncFrameQueue* inbound_ = nullptr;
    const IClock* clock_ = nullptr;
    const GattDatabase::Chars* chars_ = nullptr;

    LinkStateTracker tracker_;                 // callback-context only (no races)
    LatestValue<BleLinkState> state_mailbox_;  // the CR-018 snapshot, any-task reads
    LinkStateProvider provider_;

    // Transport-facing link flags: written by the host task, read by the
    // sync pump (ISyncTransport) - atomics, no lock, no tearing.
    std::atomic<bool> connected_{false};
    std::atomic<bool> encrypted_{false};
    std::atomic<bool> sync_subscribed_{false};
    std::atomic<uint16_t> mtu_{kDefaultAttMtu};

    // Action queue: FreeRTOS queue in static storage (created in setup(),
    // before any task runs - no heap after startup, 00 7.2).
    QueueHandle_t actions_ = nullptr;
    StaticQueue_t actions_meta_ = {};
    uint8_t actions_storage_[kGattActionQueueLen * sizeof(BleAction)] = {};

    ConnectionStats stats_ = {};  // written by the host task only
};

}  // namespace bw::ble

#endif  // ARDUINO
