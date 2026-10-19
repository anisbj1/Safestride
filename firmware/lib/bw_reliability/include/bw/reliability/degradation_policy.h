#pragma once

// bw/reliability/degradation_policy.h — the central degradation table (spec
// §6 D2 choice, §16.4): evidence in, capability flags out. One table instead
// of per-feature if/else (D2); feature logic elsewhere reads the flags.
//
// Input honesty rules (D5): every input is optional and "absent = unknown,
// NEVER down" — a missing provider snapshot or a missing SensorStatus cannot
// set a facility flag; it can only clear one (unknown is not broken).
//
// Facility flags are CURRENT capability (they clear on recovery); fault_bits
// stay sticky for the boot (D4). Pure logic: no platform calls, no locking —
// the wiring feeds it and latches the returned codes.

#include <cstdint>

#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/providers.h>  // DegradedFacility / DegradedMode / StorageHealth / BleLinkState
#include <bw/reliability/reliability_config.h>
#include <bw/reliability/watchdog_supervisor.h>  // TaskId bits in tier_b_stalled_mask
#include <bw/types.h>

namespace bw::reliability {

// DegradedFacility::Count facilities, each with at most one ENTER edge per
// step (row mask of CR-029).
constexpr uint8_t kFacilityCount = static_cast<uint8_t>(DegradedFacility::Count);

// CR-029 bit helper: bit i <=> DegradedFacility value i.
constexpr DegradedMode facilityBit(const DegradedFacility facility) {
    return static_cast<DegradedMode>(1u) << static_cast<uint8_t>(facility);
}

// D13(i): one SensorFault event per facility ENTER edge — payload[0] = the
// triggering FaultCode (None when the live status alone said Failed and no row
// code was ever reported), payload[1] = the facility bit (mask, facilityBit()).
struct FacilityEdge {
    DegradedFacility facility = DegradedFacility::PpgLost;
    FaultCode code = FaultCode::None;
};

struct PolicyInput {
    TimeMs t_ms = 0;
    uint32_t fault_bits = 0;  // CR-030 word from the registry (sticky, D4)
    bool safe_mode = false;   // BootGuard (row 7)

    // Wiring-level f01 reads (wire_diag.cpp precedent); false = absent.
    bool has_imu_status = false;
    SensorStatus imu_status = SensorStatus::Uninitialized;
    bool has_ppg_status = false;
    SensorStatus ppg_status = SensorStatus::Uninitialized;

    // CR-015 / CR-018 provider snapshots behind nullable pointers (D5).
    bool has_storage = false;
    StorageHealth storage{};
    bool has_ble = false;
    BleLinkState ble{};

    // Supervisor Tier B stalls, bit i <=> TaskId i (row 6). Tier A stalls are
    // deliberately NOT here: they gate the WDT feed (D7/D9), not a facility.
    uint32_t tier_b_stalled_mask = 0;
};

struct PolicyOutput {
    DegradedMode degraded = 0;  // CR-029 facility mask after this step
    // bit N <=> FaultCode value N: codes THIS policy produces (rows 3/4 close
    // the loop back into the registry, D5). The wiring reports them through
    // the guarded registry — the policy itself stays pure (spec §16.4 note).
    // Already-latched codes are not repeated (the registry word is sticky).
    uint32_t codes_to_report = 0;
    FacilityEdge edges[kFacilityCount]{};  // facility ENTER edges since last step
    uint8_t edge_count = 0;
};

class DegradationPolicy {
public:
    PolicyOutput step(const PolicyInput& in);

private:
    DegradedMode prev_degraded_ = 0;

    // Row 4 hysteresis: a down-timer only ever starts after an ESTABLISHED
    // link — never-connected BLE can never flag BleDown (§16.8 test 4).
    bool link_ever_connected_ = false;
    bool link_down_tracking_ = false;
    TimeMs link_down_since_ms_ = 0;

    // Row 3 "counters growing": needs the previous snapshot to compare against.
    bool has_prev_storage_ = false;
    StorageHealth prev_storage_{};
};

}  // namespace bw::reliability
