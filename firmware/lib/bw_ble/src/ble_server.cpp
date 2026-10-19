#include <bw/ble/ble_server.h>

#if defined(ARDUINO)

#include <cstring>
#include <string>

#include <bw/ble/codec.h>

namespace bw::ble {

namespace {

// [DERIVED] Advertising interval units: BLE GAP uses 0.625 ms slots, so a
// millisecond request converts with ms * 0.625^-1 = ms * 8 / 5 (D5).
uint16_t advIntervalUnits(uint32_t ms) {
    const uint32_t units = (ms * 8u) / 5u;
    if (units < 0x20u) {
        return 0x20u;  // 20 ms: the lowest slot the stack accepts reliably
    }
    if (units > 0xFFFFu) {
        return 0xFFFFu;
    }
    return static_cast<uint16_t>(units);
}

}  // namespace

BleServer::BleServer() = default;

void BleServer::bind(const Deps& deps, ConnectionMonitor* monitor, SyncTransport* transport,
                     EventNotifier* notifier, const Config& config) {
    deps_ = deps;
    monitor_ = monitor;
    transport_ = transport;
    notifier_ = notifier;
    config_ = config;
    // Cadence policy for every outgoing notification lives in the gate now
    // (audit AUD-4/AUD-6): status/battery honor Config overrides, HR is the
    // fixed 1 Hz cap, Event keeps the one-tick pacing it always had
    // (kCommsTickMs - an event must not wait longer than a tick grid).
    gate_.setIntervals({config.status_notify_interval_ms, kHrNotifyIntervalMs, kCommsTickMs,
                        config.battery_notify_interval_ms});
}

Result<void> BleServer::start() {
    if (started_) {
        return {Err::Ok};
    }
    if (!NimBLEDevice::init(config_.local_name)) {
        return {Err::HwFault};
    }
    NimBLEDevice::setMTU(config_.preferred_mtu);
    PairingManager::configure();  // D1: bonding + SC Just Works before secured chars

    server_ = NimBLEDevice::createServer();
    if (server_ == nullptr) {
        NimBLEDevice::deinit(true);
        return {Err::HwFault};
    }
    // deleteCallbacks=false is MANDATORY: the monitor lives in static
    // storage (wire_ble), and ~NimBLEServer would otherwise `delete` it on
    // deinit() - deleting a non-heap object is UB (2.5.1 [MEASURED]).
    server_->setCallbacks(monitor_, false);
    server_->advertiseOnDisconnect(true);  // checklist 2: advertising resumes

    chars_ = gatt_.create(server_);
    // Characteristic callbacks (write/CCCD) - 2.5.1 [MEASURED] requires
    // attaching per characteristic; the server callback alone never sees a
    // write. ~NimBLECharacteristic never deletes them (checked).
    for (size_t i = 0; i < kGattCharacteristicCount; ++i) {
        if (chars_.by_row[i] != nullptr) {
            chars_.by_row[i]->setCallbacks(monitor_);
        }
    }
    monitor_->setChars(&chars_);
    if (transport_ != nullptr) {
        transport_->setChars(&chars_);
    }
    fillDeviceId();
    setStaticValues();
    server_->start();

    // D5: flags + the 128-bit custom service UUID in the advertising payload
    // (companion filters by service), complete local name in the scan
    // response - the 31-byte ADV limit stays comfortable.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NimBLEUUID(gatt_.serviceUuidString()));
    // 2.5.1 [MEASURED]: enableScanResponse replaces the 1.x setScanResponse.
    // With it on, the complete local name rides the scan response, keeping
    // the 31-byte ADV payload = flags + 128-bit service UUID only.
    adv->enableScanResponse(true);
    adv->setMinInterval(advIntervalUnits(config_.advertising_interval_ms));
    adv->setMaxInterval(advIntervalUnits(config_.advertising_interval_ms));
    adv->start();

    started_ = true;
    down_reported_ = false;
    backoff_ms_ = kBleRestartInitialBackoffMs;  // healthy start resets D11
    return {Err::Ok};
}

void BleServer::stop() {
    if (!started_) {
        return;
    }
    NimBLEDevice::deinit(true);  // frees the stack; bonds live in NVS (D1)
    server_ = nullptr;
    started_ = false;
}

void BleServer::restart() {
    stop();
    next_restart_ms_ = 0;  // tick() retries immediately, then backs off (D11)
}

void BleServer::tick(TimeMs now) {
    if (!started_) {
        if (now < next_restart_ms_) {
            return;  // still backing off (D11)
        }
        if (start().ok()) {
            return;
        }
        // BleDown: the device keeps sensing, storing and alerting (00 7.1);
        // report once per down episode, then back off x2 up to the cap.
        if (!down_reported_ && deps_.faults != nullptr) {
            deps_.faults->report(FaultCode::BleDown, FaultSeverity::Warning, now);
        }
        down_reported_ = true;
        ++stats_.restarts;
        next_restart_ms_ = now + backoff_ms_;
        backoff_ms_ = backoff_ms_ * 2u;
        if (backoff_ms_ > kBleRestartMaxBackoffMs) {
            backoff_ms_ = kBleRestartMaxBackoffMs;
        }
        return;
    }

    // AUD-1 layer 2: refresh the gate's link snapshot FIRST, so every
    // publish below is evaluated against the current connected/encrypted
    // flags - an unencrypted link emits nothing (PublishGate::take).
    gate_.setLink(monitor_ != nullptr && monitor_->connected(),
                  monitor_ != nullptr && monitor_->encrypted());

    // Each publish*() asks the gate itself for permission + cadence; tick()
    // no longer paces them (a tick may arrive early on phone writes, so tick
    // count cannot enforce the HR 1 Hz cap - audit AUD-6).
    publishHeartRate(now);
    publishLiveStatus(now);
    publishEvent(now);
    publishBattery(now);
    if (now - last_config_ms_ >= kConfigRefreshIntervalMs) {
        last_config_ms_ = now;
        refreshConfig(now);
    }
}

void BleServer::handleAction(const BleAction& action, TimeMs now) {
    switch (action.kind) {
    case BleActionKind::WriteCommand: {
        const Result<Command> cmd = decodeCommand(action.data, action.len, now);
        if (!cmd.ok() || deps_.commands == nullptr || !deps_.commands->push(cmd.value)) {
            // Unknown type / malformed image / sink full: dropped AND
            // counted (spec 10) - the phone may retry, nothing is lost
            // silently. The write itself already succeeded at ATT level
            // (see BLE_GATT.md v1 3 for why semantic rejects cannot be
            // ATT errors here).
            ++stats_.command_rejected;
        }
        break;
    }
    case BleActionKind::WriteConfig: {
        if (deps_.settings == nullptr) {
            ++stats_.config_rejected;
            break;
        }
        const Result<void> applied = applyConfigWrite(*deps_.settings, config_specs_,
                                                      kMaxConfigSettings, action.data, action.len);
        if (applied.ok()) {
            refreshConfig(now);  // next read sees the new value at once
        } else {
            ++stats_.config_rejected;  // NotFound / type mismatch / bad image
        }
        break;
    }
    case BleActionKind::WriteSyncControl: {
        // Dumb pipe (CR-019 ruling 1): bytes land in the inbound queue
        // verbatim; F20 parses Request/Ack and reports protocol errors
        // through its own indications.
        if (checkSyncControlWrite(action.data, action.len).ok() && transport_ != nullptr) {
            (void)transport_->inbound().push(SyncChannel::Control, action.data, action.len);
        }
        break;
    }
    case BleActionKind::Subscribe: {
        if (notifier_ == nullptr) {
            break;
        }
        if (action.subscribed != 0) {
            notifier_->subscribe(now);  // D7: baselines, no history burst
        } else {
            notifier_->unsubscribe();  // stops all store reads (spec 9)
        }
        break;
    }
    }
}

BleStats BleServer::stats() const {
    BleStats out = stats_;
    if (monitor_ != nullptr) {
        out.actions_dropped = monitor_->stats().actions_dropped;
        out.writes_rejected = monitor_->stats().writes_rejected;
    }
    if (notifier_ != nullptr) {
        out.store_errors = notifier_->storeErrors();
    }
    // Suppressed-by-policy notifications counted by the gate (AUD-1 layer 2).
    out.notifications_dropped = gate_.insecureDrops();
    return out;
}

void BleServer::fillDeviceId() {
    // device_id = the device's own BLE address, lowercase 12-hex, no
    // separators (D4; it is the (device_id, seq) identity half of the sync
    // protocol, external.md 6.3).
    const std::string addr = NimBLEDevice::getAddress().toString();
    size_t out = 0;
    for (char c : addr) {
        if (c == ':') {
            continue;
        }
        if (out >= 12) {
            break;
        }
        device_id_[out++] = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    device_id_[out] = '\0';
}

void BleServer::setStaticValues() {
    if (chars_.by_row[kRowFwRev] != nullptr) {
        chars_.by_row[kRowFwRev]->setValue(deps_.identity.firmware_rev);
    }
    if (chars_.by_row[kRowHwRev] != nullptr) {
        chars_.by_row[kRowHwRev]->setValue(deps_.identity.hw_rev);
    }
    if (chars_.by_row[kRowSwRev] != nullptr) {
        chars_.by_row[kRowSwRev]->setValue(deps_.identity.schema_versions);
    }
    if (chars_.by_row[kRowSerial] != nullptr) {
        chars_.by_row[kRowSerial]->setValue(device_id_);
    }
    if (chars_.batteryLevel() != nullptr) {
        chars_.batteryLevel()->setValue(kBatteryUnknownPercent);  // honest 0xFF
    }
}

void BleServer::publishLiveStatus(TimeMs now) {
    // THE gate (AUD-1 layer 2): no link / not due / unencrypted -> return
    // before setValue/notify ever runs.
    if (gate_.take(NotifyChannel::LiveStatus, now) != PublishVerdict::Send) {
        return;
    }
    if (deps_.state == nullptr || chars_.liveStatus() == nullptr) {
        return;
    }
    StateSnapshot snap{};
    if (!deps_.state->latest(snap)) {
        // No snapshot yet: the characteristic value stays EMPTY - explicit
        // "no data", never a fabricated snapshot (spec D6, 00 1).
        return;
    }
    const Result<size_t> written = encodeLiveStatus(snap, live_status_, sizeof(live_status_));
    if (!written.ok()) {
        return;
    }
    chars_.liveStatus()->setValue(live_status_, written.value);
    (void)chars_.liveStatus()->notify();  // no-op while unsubscribed (D2)
}

void BleServer::publishHeartRate(TimeMs now) {
    // The 1 Hz cap lives HERE, where the sample is taken (audit AUD-6):
    // CommsTask::tick() may run more often than once per second, so the gate
    // - not the tick - is what makes "> 1 HR notify per second" impossible.
    if (gate_.take(NotifyChannel::HeartRate, now) != PublishVerdict::Send) {
        return;
    }
    if (deps_.features == nullptr || chars_.heartRate() == nullptr) {
        return;
    }
    PpgFeatures ppg{};
    if (!deps_.features->latestPpg(ppg)) {
        return;
    }
    // hr_valid == false (not worn / poor signal): NO notification - the
    // standard profile has no honest "invalid" byte (spec D6, checklist 6).
    const Result<size_t> written = encodeHeartRate(ppg, hr_, sizeof(hr_));
    if (!written.ok()) {
        return;
    }
    chars_.heartRate()->setValue(hr_, written.value);
    (void)chars_.heartRate()->notify();
}

void BleServer::publishEvent(TimeMs now) {
    // Gate FIRST, before notifier_->poll(): a dropped (unencrypted) publish
    // must not consume the notifier cursor - the event stays stored and goes
    // out on the next due, encrypted tick (audit AUD-1 layer 2).
    if (gate_.take(NotifyChannel::Event, now) != PublishVerdict::Send) {
        return;
    }
    if (notifier_ == nullptr || chars_.eventNotify() == nullptr) {
        return;
    }
    // The 48 B record needs MTU >= 51 to travel whole (kMinEventNotifyMtu):
    // below that the characteristic is suppressed, never truncated (D7).
    if (monitor_ != nullptr && monitor_->maxSendBytes() < kMaxEventNotifyBytes) {
        return;
    }
    Event newest{};
    if (!notifier_->poll(now, newest)) {
        return;
    }
    const Result<size_t> written = encodeEvent(newest, event_buf_, sizeof(event_buf_));
    if (!written.ok()) {
        return;
    }
    chars_.eventNotify()->setValue(event_buf_, written.value);
    (void)chars_.eventNotify()->notify();
}

void BleServer::publishBattery(TimeMs now) {
    if (gate_.take(NotifyChannel::Battery, now) != PublishVerdict::Send) {
        return;
    }
    if (deps_.battery == nullptr || chars_.batteryLevel() == nullptr) {
        return;
    }
    // Mapping is pure + host-tested in codec (audit AUD-7): invalid or
    // non-finite readings broadcast 0xFF "unknown", never 0%.
    const uint8_t percent = encodeBatteryLevel(deps_.battery->read());
    chars_.batteryLevel()->setValue(&percent, 1);
    (void)chars_.batteryLevel()->notify();  // no-op while unsubscribed
}

void BleServer::refreshConfig(TimeMs now) {
    (void)now;
    if (deps_.settings == nullptr || chars_.config() == nullptr) {
        return;
    }
    if (monitor_ != nullptr && !monitor_->connected()) {
        return;  // nobody is listening: skip the work entirely
    }
    const Result<size_t> written = encodeConfigDump(
        *deps_.settings, config_specs_, kMaxConfigSettings, config_dump_, sizeof(config_dump_));
    if (written.ok()) {
        chars_.config()->setValue(config_dump_, written.value);
    }
}

}  // namespace bw::ble

#endif  // ARDUINO
