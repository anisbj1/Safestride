#pragma once

// bw/ble/ble_server.h - F19 spec 7 (BleServer row) + spec D11: BLE lifecycle
// (init -> security -> GATT -> advertise), the 1 Hz cadence ticks shared by
// Live Status / HR / Event / Battery / Config, the CommsTask action drain,
// and restart-with-backoff so a dead radio never stops local monitoring.
// Device-only (NimBLE lives here and in its helper modules).

#if defined(ARDUINO)

#include <NimBLEDevice.h>

#include <bw/ble/ble_config.h>
#include <bw/ble/connection_monitor.h>
#include <bw/ble/event_notifier.h>
#include <bw/ble/gatt_database.h>
#include <bw/ble/pairing_manager.h>
#include <bw/ble/publish_gate.h>
#include <bw/ble/sync_transport.h>
#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::ble {

// Diagnostics/H-card counters (spec 10/11, docs/05 8): everything F19 can
// lose is counted, nothing is silently dropped. Documented one-by-one in
// BLE_GATT.md v1 5 (audit AUD-2) - add a counter there in the same commit.
struct BleStats {
    uint32_t restarts;          // failed start() attempts (backoff episodes)
    uint32_t command_rejected;  // bad decode or ICommandSink::push refused
    uint32_t config_rejected;   // unknown name / type mismatch / malformed
    uint32_t actions_dropped;   // action queue full (host task never blocks)
    // Counted write refusals (callback/task side) - never an ATT error: the
    // stack emits the only real ATT errors (over-max_len 0x0D, unbonded or
    // unencrypted CCCD/row access 0x05/0x0F), see BLE_GATT.md v1 3 (audit AUD-5).
    uint32_t writes_rejected;
    uint32_t store_errors;  // IEventStore failures skipped (EventNotifier)
    // Due notifications suppressed by PublishGate because the link is not
    // encrypted (audit AUD-1 layer 2).
    uint32_t notifications_dropped;
};

class BleServer {
public:
    // Constructor-injected dependencies (spec 18 / spec 3): every one may be
    // nullptr until the integrator attaches it - F19 then counts refusals
    // and sends nothing, never fabricating data (00 1).
    struct Deps {
        IStateProvider* state = nullptr;
        IFeatureProvider* features = nullptr;
        IBatteryMonitor* battery = nullptr;
        ISettings* settings = nullptr;
        ICommandSink* commands = nullptr;
        IEventStore* events = nullptr;
        IFaultReporter* faults = nullptr;
        const IClock* clock = nullptr;
        DeviceIdentity identity{};
    };

    BleServer();

    void bind(const Deps& deps, ConnectionMonitor* monitor, SyncTransport* transport,
              EventNotifier* notifier, const Config& config);

    // init -> PairingManager -> GattDatabase -> advertise (spec 7 lifecycle).
    //  Err::HwFault  NimBLE refused (stack/table/advertise failure); tick()
    //                retries with exponential backoff and reports BleDown
    //                once per episode (D11).
    Result<void> start();
    void stop();

    // F26 takeover hook (D11): drop the link/stack now; the next tick()
    // retries through the same backoff path.
    void restart();

    // One CommsTask cycle: restart backoff when down, otherwise every cadence
    // sub-tick that is due (all paced from the ONE kCommsTickMs grid, so
    // TaskHealth lateness stays meaningful - ble_config.h).
    void tick(TimeMs now);

    // One drained BleAction: decode + route (spec D8 - the bounded callback
    // half happened already; provider reads and sink pushes happen HERE, on
    // the task, never in the NimBLE host task).
    void handleAction(const BleAction& action, TimeMs now);

    bool started() const { return started_; }
    BleStats stats() const;

private:
    void fillDeviceId();
    void setStaticValues();
    void publishLiveStatus(TimeMs now);
    void publishHeartRate(TimeMs now);
    void publishEvent(TimeMs now);
    void publishBattery(TimeMs now);
    void refreshConfig(TimeMs now);

    Config config_;
    Deps deps_;
    ConnectionMonitor* monitor_ = nullptr;
    SyncTransport* transport_ = nullptr;
    EventNotifier* notifier_ = nullptr;

    GattDatabase gatt_;
    GattDatabase::Chars chars_{};
    NimBLEServer* server_ = nullptr;
    bool started_ = false;
    bool down_reported_ = false;

    // D11 backoff: x2 growth capped at kBleRestartMaxBackoffMs; F26's policy
    // supersedes when reliability lands.
    uint32_t backoff_ms_ = kBleRestartInitialBackoffMs;
    TimeMs next_restart_ms_ = 0;

    // Per-channel cadence + the encryption gate for EVERY outgoing
    // notification (audit AUD-1 layer 2 / AUD-6): intervals are set once in
    // bind() from Config + kHrNotifyIntervalMs; tick() refreshes the link
    // state; each publish*() begins with gate_.take().
    PublishGate gate_{};

    // Config dump cadence only - the notify channels moved into gate_.
    TimeMs last_config_ms_ = 0;

    // Static buffers: no heap, and no big stack arrays in the task (00 7.2,
    // spec 11 stack budget).
    uint8_t live_status_[kLiveStatusBytes] = {};
    uint8_t hr_[kHeartRateMaxBytes] = {};
    uint8_t event_buf_[kMaxEventNotifyBytes] = {};
    uint8_t config_dump_[kMaxConfigDumpBytes] = {};
    SettingSpec config_specs_[kMaxConfigSettings] = {};
    char device_id_[13] = {};  // 12 lowercase hex chars + NUL

    BleStats stats_{};
};

}  // namespace bw::ble

#endif  // ARDUINO
