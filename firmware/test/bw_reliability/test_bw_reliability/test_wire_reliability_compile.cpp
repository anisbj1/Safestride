// bw_reliability — wiring compile + seam tests (spec §16.8 item 10, plan D3/
// D10/D14). wire_reliability.cpp is compiled for the host by including it
// into THIS translation unit (bw_diag precedent): the ARDUINO-guarded task
// body and the f01/esp bindings are excluded, everything else must build and
// behave. This TU also defines reliability_test::resetWiring() — the only
// place allowed to include the wiring cpp.

#include "../../../src/app/wiring/wire_reliability.cpp"

#include <unity.h>

#include <bw/reliability/crash_record.h>
#include <bw/reliability/reliability_config.h>
#include <bw/testkit/mem_kv_store.h>

#include "reliability_test_support.h"

bw::IFaultReporter* reliability_test::resetWiring() {
    app::AppContext ctx{};
    f26::wire(ctx);  // native: graph reset + supervision table, no task
    f26::attachEventSink(nullptr);
    f26::attachKeyValueStore(nullptr);
    f26::attachStorageHealth(nullptr);
    f26::attachBleLink(nullptr);
    return ctx.faults;
}

namespace {

class FailingCrashStore final : public bw::IKeyValueStore {
public:
    bw::Result<size_t> get(const char* key, void* buf, size_t cap) override {
        if (fail_reads) {
            return {bw::Err::Busy, 0};
        }
        return backing.get(key, buf, cap);
    }
    bw::Result<void> put(const char* key, const void* data, size_t len) override {
        ++attempts;
        return fail ? bw::Result<void>{bw::Err::Busy} : backing.put(key, data, len);
    }
    bw::testkit::InMemoryKeyValueStore<4, 40, 64> backing;
    bool fail = false;
    bool fail_reads = false;
    uint32_t attempts = 0;
};

}  // namespace

void test_wire_transient_boot_read_preserves_history_until_retry(void) {
    FailingCrashStore kv;
    bw::reliability::CrashRecord seed{};
    seed.streak = bw::reliability::kMaxRebootsInWindow - 1;
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf),
                             bw::reliability::encodeCrashRecord(seed, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(kv.put(bw::reliability::kCrashRecordKey, buf, sizeof(buf)).ok());
    kv.attempts = 0;
    kv.fail_reads = true;
    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::Software, 0);
    TEST_ASSERT_EQUAL_UINT32(0, kv.attempts);  // Busy does not mean no previous boots
    f26::beat(bw::reliability::TaskId::Sensor, 0);
    f26::runTick(bw::reliability::kCrashWriteRetryMs - 1);  // even a stall must not overwrite it
    TEST_ASSERT_EQUAL_UINT32(0, kv.attempts);
    kv.fail_reads = false;
    f26::runTick(bw::reliability::kCrashWriteRetryMs);
    TEST_ASSERT_TRUE(f26::safeModeActive());
    TEST_ASSERT_EQUAL_UINT32(1, kv.attempts);
    bw::reliability::CrashRecord saved{};
    const auto got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, saved));
    TEST_ASSERT_EQUAL_UINT8(bw::reliability::kMaxRebootsInWindow, saved.streak);
}

void test_wire_failed_boot_write_retries_at_bounded_cadence(void) {
    FailingCrashStore kv;
    kv.fail = true;
    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::PowerOn, 0);
    TEST_ASSERT_EQUAL_UINT32(1, kv.attempts);

    constexpr bw::TimeMs retry = bw::reliability::kCrashWriteRetryMs;
    f26::runTick(retry - 1);
    TEST_ASSERT_EQUAL_UINT32(1, kv.attempts);
    f26::runTick(retry);  // still failing: one bounded retry, not a tick-rate loop
    TEST_ASSERT_EQUAL_UINT32(2, kv.attempts);
    kv.fail = false;
    f26::runTick(retry * 2 - 1);
    TEST_ASSERT_EQUAL_UINT32(2, kv.attempts);
    f26::runTick(retry * 2);
    TEST_ASSERT_EQUAL_UINT32(3, kv.attempts);
    f26::runTick(retry * 3);
    TEST_ASSERT_EQUAL_UINT32(3, kv.attempts);  // success cleared dirty state

    // The retried boot record survives a fresh graph and increments the streak.
    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::Software, 0);
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const auto got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    bw::reliability::CrashRecord rec{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, rec));
    TEST_ASSERT_EQUAL_UINT8(2, rec.streak);
}

void test_wire_failed_survival_write_retries_before_reboot(void) {
    FailingCrashStore kv;
    bw::reliability::CrashRecord seed{};
    seed.streak = bw::reliability::kMaxRebootsInWindow - 1;
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf),
                             bw::reliability::encodeCrashRecord(seed, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(kv.put(bw::reliability::kCrashRecordKey, buf, sizeof(buf)).ok());
    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::Software, 0);
    TEST_ASSERT_TRUE(f26::safeModeActive());

    kv.fail = true;
    constexpr bw::TimeMs survived = bw::reliability::kBootLoopBootLimitMs;
    f26::runTick(survived);  // streak clears in RAM but its write fails
    const uint32_t attempts = kv.attempts;
    kv.fail = false;
    f26::runTick(survived + bw::reliability::kCrashWriteRetryMs - 1);
    TEST_ASSERT_EQUAL_UINT32(attempts, kv.attempts);
    f26::runTick(survived + bw::reliability::kCrashWriteRetryMs);
    TEST_ASSERT_EQUAL_UINT32(attempts + 1, kv.attempts);
    TEST_ASSERT_TRUE(f26::safeModeActive());  // exit still requires reboot

    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::Software, 0);
    TEST_ASSERT_FALSE(f26::safeModeActive());  // disk contains the cleared streak
}

// D14: wire() binds the registry into AppContext (the composition reports
// through it from here on), and the output seam honestly says "no value yet"
// until the first runTick — never a fabricated snapshot (00 §1).
void test_wire_binds_registry_and_state_absent_before_tick(void) {
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    TEST_ASSERT_NOT_NULL(faults);

    bw::ReliabilityState state{};
    TEST_ASSERT_FALSE(f26::latestReliabilityState(state));
    TEST_ASSERT_FALSE(f26::stateProvider().latest(state));
    TEST_ASSERT_FALSE(f26::safeModeActive());
}

void test_wire_sensor_status_mailbox_drives_current_capability(void) {
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 0);
    f26::noteSensorStatuses(bw::SensorStatus::Ok, bw::SensorStatus::Failed);
    f26::runTick(1000);
    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_EQUAL_HEX32(bw::reliability::facilityBit(bw::DegradedFacility::PpgLost),
                            state.degraded);
    f26::noteSensorStatuses(bw::SensorStatus::Ok, bw::SensorStatus::Ok);
    f26::runTick(2000);
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_EQUAL_HEX32(0, state.degraded);  // recovered status overrides sticky fault history
    TEST_ASSERT_TRUE((state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::PpgAbsent))) !=
                     0);
}

void test_wire_watchdog_errors_remain_observable(void) {
    (void)reliability_test::resetWiring();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotReady),
                          static_cast<int>(f26::watchdogStatus()));
    f26::noteWatchdogResult(bw::Err::Ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Ok), static_cast<int>(f26::watchdogStatus()));
    f26::noteWatchdogResult(bw::Err::HwFault);
    f26::noteWatchdogResult(bw::Err::Ok);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::HwFault),
                          static_cast<int>(f26::watchdogStatus()));
}

// D10/row 7 input: an absent KV record means "no history" — this boot counts
// as streak 1, normal, and the record is written immediately at boot.
void test_wire_missing_boot_record_is_normal_boot(void) {
    bw::testkit::InMemoryKeyValueStore<4, 40, 64> kv;
    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::PowerOn, 0);

    TEST_ASSERT_FALSE(f26::safeModeActive());  // 1 fast boot is normal

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const bw::Result<size_t> got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    bw::reliability::CrashRecord rec{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, rec));
    TEST_ASSERT_EQUAL_UINT8(1, rec.streak);  // written at boot (D10)
}

// Row 7 end to end: a record persisting streak 2 -> this boot is #3 -> safe
// mode, the BootLoop code is latched (so fault_bits show it) and the record
// is rewritten with the new streak.
void test_wire_streak_accumulates_to_safe_mode_and_latches_bootloop(void) {
    bw::testkit::InMemoryKeyValueStore<4, 40, 64> kv;
    bw::reliability::CrashRecord seed{};
    seed.streak = 2;
    seed.last_reset_reason = bw::reliability::ResetReason::TaskWatchdog;
    uint8_t seed_buf[bw::reliability::kCrashRecordBytes];
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kCrashRecordBytes,
                             bw::reliability::encodeCrashRecord(seed, seed_buf, sizeof(seed_buf)));
    TEST_ASSERT_TRUE(kv.put(bw::reliability::kCrashRecordKey, seed_buf, sizeof(seed_buf)).ok());

    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::TaskWatchdog, 0);
    TEST_ASSERT_TRUE(f26::safeModeActive());

    f26::runTick(1000);  // publish the state once

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_TRUE(state.safe_mode);
    TEST_ASSERT_TRUE((state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::BootLoop))) !=
                     0);  // row 7 code latched
    TEST_ASSERT_TRUE(
        (state.degraded & bw::reliability::facilityBit(bw::DegradedFacility::SafeMode)) != 0);
    // Row 8 side effect: the anomalous reset reason also latched its code.
    TEST_ASSERT_TRUE(
        (state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::WatchdogReset))) != 0);

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const bw::Result<size_t> got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    bw::reliability::CrashRecord rec{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, rec));
    TEST_ASSERT_EQUAL_UINT8(3, rec.streak);  // 2 persisted + this boot
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::reliability::ResetReason::TaskWatchdog),
                          static_cast<int>(rec.last_reset_reason));
}

// §16.8 item 3, KV half: a schema-mismatched record is ABSENT — streak base
// 0 (normal boot), and the corrupt bytes are replaced by a valid record.
void test_wire_corrupt_boot_record_treated_as_absent(void) {
    bw::testkit::InMemoryKeyValueStore<4, 40, 64> kv;
    uint8_t junk[bw::reliability::kCrashRecordBytes];
    for (size_t i = 0; i < sizeof(junk); ++i) {
        junk[i] = 0x5A;
    }
    junk[0] = 0x99;  // wrong schema byte
    TEST_ASSERT_TRUE(kv.put(bw::reliability::kCrashRecordKey, junk, sizeof(junk)).ok());

    (void)reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::Software, 0);
    TEST_ASSERT_FALSE(f26::safeModeActive());  // garbage never counts a streak

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const bw::Result<size_t> got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    bw::reliability::CrashRecord rec{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, rec));  // rewritten ok
    TEST_ASSERT_EQUAL_UINT8(1, rec.streak);
    TEST_ASSERT_EQUAL_UINT8(bw::reliability::kCrashRecordSchema, buf[0]);
}

// Native shape of gatherInput (wire_diag precedent): no f01 statuses and no
// providers on the host => unknown inputs produce NO flags (D5), while a
// feature-reported code flows into the published state through ctx.faults.
void test_wire_run_tick_native_shape_is_honest(void) {
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    faults->report(bw::FaultCode::LowHeap, bw::FaultSeverity::Warning, 100);

    f26::runTick(1000);

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_EQUAL_UINT64(1000, state.t_ms);
    TEST_ASSERT_EQUAL_HEX32(0, state.degraded);  // absent != down
    TEST_ASSERT_FALSE(state.safe_mode);
    TEST_ASSERT_TRUE((state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::LowHeap))) !=
                     0);
}

// Row 5 wiring: a beatless Tier A task crosses 3x its period => TaskStalled
// reported at enter (Critical path) AND the crash record is persisted with
// the latched bits for the next boot (spec §16.4 row 5 side effects).
void test_wire_tier_a_stall_reports_and_persists_crash_record(void) {
    bw::testkit::InMemoryKeyValueStore<4, 40, 64> kv;
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    f26::attachKeyValueStore(&kv);
    f26::noteBoot(bw::reliability::ResetReason::PowerOn, 0);
    // LowHeap is a BITS-ONLY code (row 9-10 family): it must ride through to
    // the published state and the crash record without creating any facility.
    faults->report(bw::FaultCode::LowHeap, bw::FaultSeverity::Warning, 10);

    f26::beat(bw::reliability::TaskId::Sensor, 0);
    f26::runTick(10000);  // Sensor deadline = 3 x 20 ms -> long stalled

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    const uint32_t task_bit = 1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled);
    TEST_ASSERT_TRUE((state.fault_bits & task_bit) != 0);
    // Tier A stall is not a facility (policy row 5): flag stays clean.
    TEST_ASSERT_EQUAL_HEX32(0, state.degraded);

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const bw::Result<size_t> got = kv.get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    TEST_ASSERT_TRUE(got.ok());
    bw::reliability::CrashRecord rec{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, got.value, rec));
    TEST_ASSERT_TRUE((rec.fault_bits_at_boot & task_bit) != 0);
    TEST_ASSERT_TRUE(
        (rec.fault_bits_at_boot & (1u << static_cast<uint8_t>(bw::FaultCode::LowHeap))) != 0);

    // Enter edge only: a second tick with no new transition does not rewrite
    // with extra counts (the record carries bits, not counts — D11).
    f26::beat(bw::reliability::TaskId::Sensor, 10000);
    f26::runTick(11000);
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_TRUE((state.fault_bits & task_bit) != 0);
}

// Row 6 wiring: dead Tier B tasks map to their facility flags (Storage/Comms)
// while Diag/Ui/ServicePump stay bits-only; Tier A stays pending (never
// beaten is not stalled).
void test_wire_tier_b_stalls_map_to_facility_flags(void) {
    (void)reliability_test::resetWiring();
    f26::beat(bw::reliability::TaskId::Storage, 0);
    f26::beat(bw::reliability::TaskId::Comms, 0);
    f26::beat(bw::reliability::TaskId::Ui, 0);
    // 16 000 ms > 3 x each deadline: Storage 3000, Comms 15 000, Ui 300.
    f26::runTick(16000);

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_TRUE((state.degraded &
                      bw::reliability::facilityBit(bw::DegradedFacility::StorageDegraded)) != 0);
    TEST_ASSERT_TRUE(
        (state.degraded & bw::reliability::facilityBit(bw::DegradedFacility::BleDown)) != 0);
    TEST_ASSERT_EQUAL_HEX32(0, state.degraded &
                                   bw::reliability::facilityBit(bw::DegradedFacility::SafeMode));
    TEST_ASSERT_TRUE(
        (state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled))) != 0);
    // Row 6: the mapped flag itself is NOT re-reported as its own code.
    TEST_ASSERT_EQUAL_HEX32(0, state.fault_bits &
                                   (1u << static_cast<uint8_t>(bw::FaultCode::StorageDegraded)));
}

// CR-033(a): a SupervisorTask that failed xTaskCreate* latches
// FaultCode::TaskCreateFailed (27), NOT the old TaskStalled stand-in — a task
// that never started never had a heartbeat, so the two symptoms must stay
// distinguishable (CR-033 alternative (a)). Reverting the report site to
// TaskStalled fails this test.
void test_wire_task_create_failure_latches_task_create_failed(void) {
    (void)reliability_test::resetWiring();
    f26::reportTaskCreateFailure(500);

    // Grace window (kSupervisionGraceMs): nothing is stalled yet, so the only
    // bit in the published state is the one the creation failure latched.
    f26::runTick(1000);

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_TRUE(
        (state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::TaskCreateFailed))) != 0);
    TEST_ASSERT_EQUAL_HEX32(0, state.fault_bits &
                                   (1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled)));
}

// D12 seam: one ladder per sensor kind, and a sentinel argument falls back to
// the IMU ladder instead of indexing out of range (unwired by G1 ruling 3).
void test_wire_reinit_seam_returns_distinct_ladders(void) {
    (void)reliability_test::resetWiring();
    bw::reliability::ReinitScheduler& imu = f26::reinitScheduler(bw::reliability::SensorKind::Imu);
    bw::reliability::ReinitScheduler& ppg = f26::reinitScheduler(bw::reliability::SensorKind::Ppg);
    bw::reliability::ReinitScheduler& sentinel =
        f26::reinitScheduler(bw::reliability::SensorKind::Count);

    TEST_ASSERT_TRUE(&imu != &ppg);
    TEST_ASSERT_TRUE(&sentinel == &imu);  // sentinel -> IMU's ladder
}
