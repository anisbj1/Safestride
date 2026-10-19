// bw_reliability — DegradationPolicy tests (spec §16.8 item 4): ONE case per
// §16.4 row (1–10), plus the D5 honesty rules (absent = unknown, never down),
// row 4's confirm hysteresis and the never-connected guarantee. Pure logic:
// input in, DegradedMode mask + edges + codes_to_report out.

#include <bw/fault.h>
#include <bw/providers.h>
#include <bw/reliability/degradation_policy.h>
#include <bw/reliability/reliability_config.h>
#include <bw/reliability/watchdog_supervisor.h>

#include <unity.h>

#include "reliability_test_support.h"

namespace {

using bw::DegradedFacility;
using bw::DegradedMode;
using bw::SensorStatus;
using bw::reliability::DegradationPolicy;
using bw::reliability::FacilityEdge;
using bw::reliability::PolicyInput;
using bw::reliability::PolicyOutput;

constexpr DegradedMode bit(const DegradedFacility facility) {
    return bw::reliability::facilityBit(facility);
}

constexpr uint32_t codeBit(const bw::FaultCode code) {
    return 1u << static_cast<uint8_t>(code);
}

uint32_t taskBit(const bw::reliability::TaskId id) {
    return 1u << static_cast<uint8_t>(id);
}

// The first facility ENTER edge, or a zeroed sentinel when there is none.
FacilityEdge firstEdge(const PolicyOutput& out) {
    return out.edge_count > 0 ? out.edges[0] : FacilityEdge{};
}

}  // namespace

// --- row 1 (PPG) -------------------------------------------------------------

// Live status is authoritative (D15): Failed sets PpgLost with exactly one
// ENTER edge; the triggering code is None here because no row code was ever
// latched — honest "why" (payload[0] semantics, D13(i)).
void test_policy_row1_ppg_failed_sets_flag_with_edge(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_ppg_status = true;
    in.ppg_status = SensorStatus::Failed;

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::PpgLost), out.degraded);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DegradedFacility::PpgLost),
                          static_cast<int>(firstEdge(out).facility));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(firstEdge(out).code));
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);  // row 1 reports no own code
}

// D15 lifecycle: the flag is CURRENT capability — a recovered PPG clears it
// even though the sticky fault bits still carry PpgAbsent (bits never clear,
// flags must).
void test_policy_row1_recovery_clears_flag_despite_sticky_bits(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_ppg_status = true;
    in.ppg_status = SensorStatus::Failed;
    in.fault_bits = codeBit(bw::FaultCode::PpgAbsent);  // latched earlier this boot
    (void)p.step(in);

    in.ppg_status = SensorStatus::Ok;
    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);  // exit edges do not exist
}

// No live status (host / F01 not wired): the latched row codes are the only
// evidence and DO carry the flag, with the first latched one as the cause.
void test_policy_row1_absent_status_uses_latched_codes(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);  // has_ppg_status = false
    in.fault_bits = codeBit(bw::FaultCode::PpgFlatline) | codeBit(bw::FaultCode::PpgReadFailed);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::PpgLost), out.degraded);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);
    // First latched in ROW order (§16.4 row 1: PpgAbsent, PpgReadFailed,
    // PpgFlatline, ...), not bit order.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgReadFailed),
                          static_cast<int>(firstEdge(out).code));
}

// --- row 2 (IMU) -------------------------------------------------------------

// Failed IMU => ImuLost + edge; recovery clears it (same D15 rule as row 1).
void test_policy_row2_imu_failed_sets_and_recovers(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_imu_status = true;
    in.imu_status = SensorStatus::Failed;
    in.fault_bits = codeBit(bw::FaultCode::ImuStuckSensor);

    PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::ImuLost), out.degraded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(firstEdge(out).code));

    in.imu_status = SensorStatus::Ok;
    out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
}

// --- row 3 (storage) ---------------------------------------------------------

// corrupt_records > 0 => StorageDegraded flag AND the row's own code once
// (D5 loop close: codes_to_report only when not already latched).
void test_policy_row3_corrupt_records_flags_and_reports_once(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_storage = true;
    in.storage = reliability_test::corruptStorage(1000);

    PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::StorageDegraded), out.degraded);
    TEST_ASSERT_TRUE((out.codes_to_report & codeBit(bw::FaultCode::StorageDegraded)) != 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::StorageDegraded),
                          static_cast<int>(firstEdge(out).code));

    // The wiring latched the code into the registry; next step must not
    // report it again (once per boot).
    in.fault_bits = codeBit(bw::FaultCode::StorageDegraded);
    out = p.step(in);
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);  // flag held, no new edge
}

// Low free space (< kStorageDegradedFreePct) and a growing torn-tail REPAIR
// counter both trigger; healthy eviction/kv-discard growth must NOT (F18
// behaviour is not degradation — spec §16.4 note).
void test_policy_row3_low_free_and_torn_growth_but_not_eviction(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_storage = true;
    in.storage = reliability_test::lowFreeStorage(1000);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::StorageDegraded), p.step(in).degraded);

    // Healthy counters first: no flag while eviction grows.
    in.storage = reliability_test::healthyStorage(2000);
    in.storage.evicted_windows = 10;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    in.storage.evicted_windows = 99;
    in.storage.kv_discards = 7;  // healthy F18 discards, not corruption
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);

    // Torn-tail repairs appearing since the previous snapshot => flag.
    in.storage = reliability_test::healthyStorage(3000);
    in.storage.torn_tails_repaired = 2;
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::StorageDegraded), p.step(in).degraded);
}

// Row 6 (Storage part): a dead StorageTask maps to the row 3 FLAG but the
// only code is TaskStalled — no StorageDegraded code is invented.
void test_policy_row3_storage_task_stall_maps_flag_without_own_code(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.tier_b_stalled_mask = taskBit(bw::reliability::TaskId::Storage);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::StorageDegraded), out.degraded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::TaskStalled),
                          static_cast<int>(firstEdge(out).code));
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}

// --- row 4 (BLE) -------------------------------------------------------------

// A never-connected phone can NEVER flag BleDown, no matter how long (the
// confirm window only starts after an established link — §16.8 test 4).
void test_policy_row4_never_connected_never_flags(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_ble = true;
    in.ble.t_ms = 1000;
    in.ble.connected = false;

    PolicyOutput out{};
    for (bw::TimeMs t = 1000; t <= 120000; t += 30000) {
        in.t_ms = t;
        in.ble.t_ms = t;
        out = p.step(in);
    }
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}

// Established link, then a confirmed loss: nothing before
// kBleDownConfirmMs, flag + code + edge exactly at the boundary (D5 code
// latch included).
void test_policy_row4_loss_after_connection_confirms_at_boundary(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_ble = true;
    in.ble.connected = true;
    in.ble.t_ms = 1000;
    (void)p.step(in);  // established: link_ever_connected_

    // Disconnected from t=1000: timer starts.
    in.ble.connected = false;
    in.t_ms = 1000;
    in.ble.t_ms = 1000;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);  // just started

    in.t_ms = 1000 + bw::reliability::kBleDownConfirmMs - 1;
    in.ble.t_ms = in.t_ms;  // fresh snapshot every step
    PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);  // one ms short is still not confirmed

    in.t_ms += 1;
    in.ble.t_ms = in.t_ms;
    out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::BleDown), out.degraded);
    TEST_ASSERT_TRUE((out.codes_to_report & codeBit(bw::FaultCode::BleDown)) != 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::BleDown),
                          static_cast<int>(firstEdge(out).code));
}

// Future-dated snapshots cannot be current evidence. This replaces the old
// age-based test: CR-018 timestamps link EVENTS, not periodic heartbeats.
void test_policy_row4_future_snapshot_is_unknown_not_down(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.has_ble = true;
    in.ble.connected = true;
    in.ble.t_ms = 1000;
    (void)p.step(in);

    // Disconnected, timer running from t=1000...
    in.ble.connected = false;
    in.t_ms = 1000;
    in.ble.t_ms = 1000;
    (void)p.step(in);

    // ...but the provider returns evidence from the future. Never confirm it.
    in.t_ms = 60000;
    in.ble.t_ms = in.t_ms + 1;
    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}

void test_policy_row4_held_event_time_snapshot_confirms_loss(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(10000);
    in.has_ble = true;
    in.ble.t_ms = 0;  // connection event predates polling; F19 holds this timestamp
    in.ble.connected = true;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    in.ble.connected = false;
    in.ble.t_ms = in.t_ms;
    (void)p.step(in);
    in.t_ms += bw::reliability::kBleDownConfirmMs - 1;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    ++in.t_ms;  // leave ble.t_ms unchanged, exactly as the real producer does
    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::BleDown), out.degraded);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);
    TEST_ASSERT_TRUE((out.codes_to_report & codeBit(bw::FaultCode::BleDown)) != 0);
    in.ble.connected = true;
    in.ble.t_ms = in.t_ms;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);  // reconnect clears current capability flag
}

void test_policy_row4_missing_snapshot_breaks_confirmation(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(0);
    in.has_ble = true;
    in.ble.connected = true;
    (void)p.step(in);
    in.ble.connected = false;
    (void)p.step(in);
    ++in.t_ms;
    in.has_ble = false;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    in.t_ms += bw::reliability::kBleDownConfirmMs;
    in.has_ble = true;
    in.ble.t_ms = in.t_ms;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);  // unknown time doesn't confirm a loss
    in.t_ms += bw::reliability::kBleDownConfirmMs;
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::BleDown), p.step(in).degraded);
}

void test_policy_row4_backward_time_restarts_confirmation(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(bw::reliability::kBleDownConfirmMs);
    in.has_ble = true;
    in.ble.connected = true;
    (void)p.step(in);
    in.ble.connected = false;
    (void)p.step(in);
    in.t_ms = 0;  // do not unsigned-underflow into an instantly confirmed loss
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    in.t_ms = bw::reliability::kBleDownConfirmMs - 1;
    TEST_ASSERT_EQUAL_HEX32(0, p.step(in).degraded);
    ++in.t_ms;
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::BleDown), p.step(in).degraded);
}

// Row 6 (BLE part): a dead CommsTask maps to the BleDown flag (capability is
// down) but latches only TaskStalled — no BleDown code without a link event.
void test_policy_row4_comms_task_stall_maps_flag_without_own_code(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.tier_b_stalled_mask = taskBit(bw::reliability::TaskId::Comms);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::BleDown), out.degraded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::TaskStalled),
                          static_cast<int>(firstEdge(out).code));
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}

// --- rows 5–10 + honesty rules ----------------------------------------------

// Row 7: safe mode => SafeMode flag with BootLoop as the edge cause; the
// table itself never sets fault bits (the wiring reports row 7's code).
void test_policy_row7_safe_mode_flagged_with_bootloop_cause(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.safe_mode = true;

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(bit(DegradedFacility::SafeMode), out.degraded);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::BootLoop),
                          static_cast<int>(firstEdge(out).code));
}

// Rows 9/10: battery and I2C codes are BITS ONLY — no facility flag, no edge,
// no code from this policy (their owners act on them; row 10 only drives
// rows 1/2 outcomes through their own codes).
void test_policy_rows9_10_battery_and_i2c_are_bits_only(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.fault_bits = codeBit(bw::FaultCode::CriticalBattery) | codeBit(bw::FaultCode::LowBattery) |
                    codeBit(bw::FaultCode::I2cTimeout) | codeBit(bw::FaultCode::I2cBusStuck);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}

// Row 5 is deliberately NOT a facility input: Tier A stalls gate the WDT feed
// (D7/D9), so even a Tier A bit arriving here must produce no flag.
void test_policy_row5_tier_a_stall_is_not_a_facility(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.tier_b_stalled_mask =
        taskBit(bw::reliability::TaskId::Sensor) | taskBit(bw::reliability::TaskId::Alert);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
}

// Row 6 (Diag/Ui/ServicePump part): those stalls are bits-only — no mapped
// facility flag (their own service degrades, spec §16.4 row 6).
void test_policy_row6_diag_ui_pump_stalls_are_bits_only(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);
    in.tier_b_stalled_mask = taskBit(bw::reliability::TaskId::Diag) |
                             taskBit(bw::reliability::TaskId::Ui) |
                             taskBit(bw::reliability::TaskId::ServicePump);

    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
}

// D5 honesty, global: every absent provider leaves the table empty — unknown
// must never become "down" (00 §1: unknown is a valid output).
void test_policy_absent_inputs_produce_no_flags(void) {
    DegradationPolicy p;
    PolicyInput in = reliability_test::baseInput(1000);  // nothing present
    const PolicyOutput out = p.step(in);
    TEST_ASSERT_EQUAL_HEX32(0, out.degraded);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_EQUAL_UINT32(0, out.codes_to_report);
}
