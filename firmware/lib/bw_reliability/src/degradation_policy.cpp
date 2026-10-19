#include <bw/reliability/degradation_policy.h>

namespace bw::reliability {

namespace {

// Facility indices (DegradedFacility values, CR-029 append-only).
constexpr uint8_t kPpgIdx = static_cast<uint8_t>(DegradedFacility::PpgLost);
constexpr uint8_t kImuIdx = static_cast<uint8_t>(DegradedFacility::ImuLost);
constexpr uint8_t kStorageIdx = static_cast<uint8_t>(DegradedFacility::StorageDegraded);
constexpr uint8_t kBleIdx = static_cast<uint8_t>(DegradedFacility::BleDown);
constexpr uint8_t kSafeIdx = static_cast<uint8_t>(DegradedFacility::SafeMode);

// Row 6 task bits (bit i <=> TaskId i).
constexpr uint32_t kStorageTaskBit = 1u << static_cast<uint8_t>(TaskId::Storage);
constexpr uint32_t kCommsTaskBit = 1u << static_cast<uint8_t>(TaskId::Comms);

// §16.4 row code lists: a LATCHED code here means the source is unusable when
// no live SensorStatus is available (D15, spec §16.4 note). Order = row order;
// first latched one becomes the event's triggering code.
constexpr FaultCode kPpgLostCodes[] = {
    FaultCode::PpgAbsent,    FaultCode::PpgReadFailed,  FaultCode::PpgFlatline,
    FaultCode::PpgSaturated, FaultCode::PpgPoorContact,
};
constexpr FaultCode kImuLostCodes[] = {
    FaultCode::ImuAbsent,
    FaultCode::ImuReadFailed,
    FaultCode::ImuStuckSensor,
};

FaultCode firstLatched(const FaultCode* codes, const size_t count, const uint32_t bits) {
    for (size_t i = 0; i < count; ++i) {
        const uint8_t value = static_cast<uint8_t>(codes[i]);
        if (value < 32 && (bits & (static_cast<uint32_t>(1u) << value)) != 0) {
            return codes[i];
        }
    }
    return FaultCode::None;
}

constexpr uint32_t codeBit(const FaultCode code) {
    return static_cast<uint32_t>(1u) << static_cast<uint8_t>(code);
}

}  // namespace

PolicyOutput DegradationPolicy::step(const PolicyInput& in) {
    PolicyOutput out{};
    FaultCode causes[kFacilityCount]{};
    DegradedMode degraded = 0;

    // ---- row 1: PPG ---------------------------------------------------------
    // D15 (spec §16.4): the live SensorStatus is authoritative when present —
    // sticky row codes must not make PpgLost irreversible (bits stay latched,
    // the flag describes CURRENT capability). Codes carry the flag only when
    // the wiring cannot read a status (absent = codes are the only evidence).
    if (in.has_ppg_status) {
        if (in.ppg_status == SensorStatus::Failed) {
            degraded |= facilityBit(DegradedFacility::PpgLost);
            causes[kPpgIdx] = firstLatched(kPpgLostCodes, sizeof(kPpgLostCodes) / sizeof(FaultCode),
                                           in.fault_bits);
        }
    } else {
        const FaultCode cause =
            firstLatched(kPpgLostCodes, sizeof(kPpgLostCodes) / sizeof(FaultCode), in.fault_bits);
        if (cause != FaultCode::None) {
            degraded |= facilityBit(DegradedFacility::PpgLost);
            causes[kPpgIdx] = cause;
        }
    }

    // ---- row 2: IMU (same rule as row 1) ------------------------------------
    if (in.has_imu_status) {
        if (in.imu_status == SensorStatus::Failed) {
            degraded |= facilityBit(DegradedFacility::ImuLost);
            causes[kImuIdx] = firstLatched(kImuLostCodes, sizeof(kImuLostCodes) / sizeof(FaultCode),
                                           in.fault_bits);
        }
    } else {
        const FaultCode cause =
            firstLatched(kImuLostCodes, sizeof(kImuLostCodes) / sizeof(FaultCode), in.fault_bits);
        if (cause != FaultCode::None) {
            degraded |= facilityBit(DegradedFacility::ImuLost);
            causes[kImuIdx] = cause;
        }
    }

    // ---- row 3: storage (CR-015) -------------------------------------------
    // "counters growing" (spec §16.4) = a torn-tail REPAIR appearing since the
    // previous snapshot: torn writes are the slow-moving harm signal. Normal
    // eviction/kv-discard growth is healthy F18 behaviour, not degradation —
    // it must not flag the store (spec §16.4 note).
    bool storage_cond = false;
    if (in.has_storage) {
        const StorageHealth& h = in.storage;
        const bool low_free =
            h.total_bytes > 0 &&
            (static_cast<uint64_t>(h.free_bytes) * 100u) / h.total_bytes < kStorageDegradedFreePct;
        const bool torn_grew =
            has_prev_storage_ && h.torn_tails_repaired > prev_storage_.torn_tails_repaired;
        storage_cond = h.corrupt_records > 0 || h.segments_skipped > 0 || torn_grew || low_free;
        prev_storage_ = h;
        has_prev_storage_ = true;
    }
    const bool storage_stalled = (in.tier_b_stalled_mask & kStorageTaskBit) != 0;
    if (storage_cond) {
        degraded |= facilityBit(DegradedFacility::StorageDegraded);
        causes[kStorageIdx] = FaultCode::StorageDegraded;
        if ((in.fault_bits & codeBit(FaultCode::StorageDegraded)) == 0) {
            out.codes_to_report |= codeBit(FaultCode::StorageDegraded);  // D5: once per boot
        }
    } else if (storage_stalled) {
        // Row 6: the StorageTask is dead — capability is down, but the only
        // code reported is TaskStalled (the supervisor already latched it).
        degraded |= facilityBit(DegradedFacility::StorageDegraded);
        causes[kStorageIdx] = FaultCode::TaskStalled;
    }

    // ---- row 4: BLE (CR-018) ------------------------------------------------
    // CR-018 is an event-time snapshot, not a periodic heartbeat: unchanged
    // link state remains authoritative regardless of timestamp age. Producer
    // liveness is supervised separately through CommsTask (row 6). An absent
    // or future-dated snapshot is unknown and breaks continuous-loss evidence.
    bool ble_cond = false;
    if (in.has_ble && in.ble.t_ms <= in.t_ms) {
        if (in.ble.connected) {
            link_ever_connected_ = true;
            link_down_tracking_ = false;
        } else if (link_ever_connected_ &&
                   (!link_down_tracking_ || in.t_ms < link_down_since_ms_)) {
            link_down_tracking_ = true;
            link_down_since_ms_ = in.t_ms;
        }
        ble_cond = link_down_tracking_ && (in.t_ms - link_down_since_ms_ >= kBleDownConfirmMs);
    } else {
        link_down_tracking_ = false;  // unknown ≠ down; never bridge a missing-provider gap
    }
    const bool comms_stalled = (in.tier_b_stalled_mask & kCommsTaskBit) != 0;
    if (ble_cond) {
        degraded |= facilityBit(DegradedFacility::BleDown);
        causes[kBleIdx] = FaultCode::BleDown;
        if ((in.fault_bits & codeBit(FaultCode::BleDown)) == 0) {
            out.codes_to_report |= codeBit(FaultCode::BleDown);  // D5: once per boot
        }
    } else if (comms_stalled) {
        degraded |= facilityBit(DegradedFacility::BleDown);  // row 6: mapped flag
        causes[kBleIdx] = FaultCode::TaskStalled;
    }

    // ---- row 7: safe mode ---------------------------------------------------
    if (in.safe_mode) {
        degraded |= facilityBit(DegradedFacility::SafeMode);
        causes[kSafeIdx] = FaultCode::BootLoop;
    }

    // ---- rows 8/9/10 and row 6's Diag/Ui/ServicePump ------------------------
    // deliberately produce NO facility flag: WatchdogReset/Brownout,
    // Low/CriticalBattery and I2cTimeout/I2cBusStuck live only in fault_bits
    // (reported by their owners), and a dead Diag/Ui/ServicePump degrades its
    // own service — bits only, per §16.4.

    // ---- ENTER edges (D13(i)) ----------------------------------------------
    for (uint8_t i = 0; i < kFacilityCount; ++i) {
        const DegradedMode bit = static_cast<DegradedMode>(1u) << i;
        if ((degraded & bit) != 0 && (prev_degraded_ & bit) == 0) {
            out.edges[out.edge_count++] = FacilityEdge{static_cast<DegradedFacility>(i), causes[i]};
        }
    }

    prev_degraded_ = degraded;
    out.degraded = degraded;
    return out;
}

}  // namespace bw::reliability
