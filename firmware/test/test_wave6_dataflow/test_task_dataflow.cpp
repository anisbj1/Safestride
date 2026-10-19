// Compile the REAL wiring on host, as in the feature wiring suites. All
// platform task bodies stay device-only; the mapping/report/provider seams
// below are the ones those bodies call, not substitute implementations.
#include "../../src/app/wiring/wire_diag.cpp"
#include "../../src/app/wiring/wire_reliability.cpp"

#include <bw/alert/alert_policy.h>
#include <bw/ble/link_state_tracker.h>
#include <bw/testkit/mem_kv_store.h>
#include <unity.h>

namespace {
using bw::diag::TaskSlot;
using bw::reliability::BeatState;
using bw::reliability::TaskId;

struct Coverage {
    TaskSlot slot;
    TaskId id;
    uint32_t period_ms;
};

// [DERIVED] One row per scheduled producer, excluding Supervisor (owns feed).
constexpr Coverage kCoverage[] = {
    {TaskSlot::Sensor, TaskId::Sensor, 20},
    {TaskSlot::Processing, TaskId::Processing, 1000},
    {TaskSlot::Intelligence, TaskId::Intelligence, 1000},
    {TaskSlot::State, TaskId::State, 1000},
    {TaskSlot::Alert, TaskId::Alert, 50},
    {TaskSlot::Input, TaskId::Input, 10},
    {TaskSlot::DiagSelf, TaskId::Diag, 1000},
    {TaskSlot::Ui, TaskId::Ui, 100},
    {TaskSlot::Storage, TaskId::Storage, 1000},
    {TaskSlot::Comms, TaskId::Comms, 1000},
    {TaskSlot::ServicePump, TaskId::ServicePump, 2000},
    {TaskSlot::Wear, TaskId::Wear, 1000},
    {TaskSlot::Wake, TaskId::Wake, 1000},
    {TaskSlot::Sleep, TaskId::Sleep, 1000},
    {TaskSlot::Cataplexy, TaskId::Cataplexy, 1000},
    {TaskSlot::EventLog, TaskId::EventLog, 100},
    {TaskSlot::Baseline, TaskId::Baseline, 1000},
    {TaskSlot::Research, TaskId::Research, 10},
};
static_assert(sizeof(kCoverage) / sizeof(kCoverage[0]) == static_cast<size_t>(TaskId::Count));

app::AppContext& resetGraph() {
    static app::AppContext ctx{};
    // Detach before reset: a previous test's stack-owned provider is gone.
    f26::attachEventSink(nullptr);
    f26::attachKeyValueStore(nullptr);
    f26::attachStorageHealth(nullptr);
    f26::attachBleLink(nullptr);
    f26::wire(ctx);
    f23::attachReliabilityProvider(&f26::stateProvider());
    return ctx;
}

constexpr uint32_t faultBit(bw::FaultCode code) {
    return 1u << static_cast<uint8_t>(code);
}

bw::HealthSnapshot composeHealth(app::AppContext& ctx, bw::TimeMs now) {
    f26::runTick(now);
    f23::runTick(ctx, f23::gatherInput(ctx, now));
    bw::HealthSnapshot health{};
    TEST_ASSERT_TRUE(f23::latestHealth(health));
    return health;
}

void assertNeighborCannotMask(TaskSlot healthy, TaskSlot dead, TaskId dead_id) {
    auto& ctx = resetGraph();
    app::tasks::recordCycle(dead, 0, 0, 0);
    app::tasks::recordCycle(healthy, 0, 0, 0);
    f26::runTick(0);
    // [DERIVED] One millisecond beyond the shared 1 Hz stall boundary.
    constexpr bw::TimeMs kStallAtMs = bw::reliability::kStallFactor * 1000 + 1;
    app::tasks::recordCycle(healthy, kStallAtMs, 0, 0);
    const auto health = composeHealth(ctx, kStallAtMs);
    TEST_ASSERT_EQUAL(BeatState::Stalled, f26::s_supervisor.state(dead_id));
    TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::TaskStalled), health.fault_bits);
}
}  // namespace

void test_each_task_has_independent_health_and_supervision(void) {
    for (const auto& row : kCoverage) {
        auto& ctx = resetGraph();
        app::tasks::recordCycle(row.slot, 0, 1, 2048);
        f26::runTick(0);
        TEST_ASSERT_EQUAL(BeatState::Fresh, f26::s_supervisor.state(row.id));
        bw::TaskHealth task{};
        TEST_ASSERT_TRUE(f23::latestTaskHealth(row.slot, task));
        TEST_ASSERT_TRUE(task.valid);
        TEST_ASSERT_EQUAL_UINT16(2048, task.stack_free_min_bytes);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(1, task.worst_lateness_ms);
        const bw::TimeMs stall_at = row.period_ms * bw::reliability::kStallFactor + 1;
        const auto health = composeHealth(ctx, stall_at);
        TEST_ASSERT_EQUAL(BeatState::Stalled, f26::s_supervisor.state(row.id));
        TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::TaskStalled), health.fault_bits);
    }
    app::tasks::recordCycle(TaskSlot::Supervisor, 0, 0, 2048);
    bw::TaskHealth supervisor{};
    TEST_ASSERT_TRUE(f23::latestTaskHealth(TaskSlot::Supervisor, supervisor));
    app::tasks::recordCycle(TaskSlot::Count, 0, 0, 0);
    app::tasks::recordCycle(static_cast<TaskSlot>(UINT8_MAX), 0, 0, 0);
    TEST_ASSERT_FALSE(f23::latestTaskHealth(TaskSlot::Count, supervisor));
}

void test_healthy_activity_cannot_mask_stalled_wear_or_wake(void) {
    assertNeighborCannotMask(TaskSlot::Intelligence, TaskSlot::Wear, TaskId::Wear);
    assertNeighborCannotMask(TaskSlot::Intelligence, TaskSlot::Wake, TaskId::Wake);
}

void test_healthy_state_cannot_mask_stalled_sleep_or_cataplexy(void) {
    assertNeighborCannotMask(TaskSlot::State, TaskSlot::Sleep, TaskId::Sleep);
    assertNeighborCannotMask(TaskSlot::State, TaskSlot::Cataplexy, TaskId::Cataplexy);
}

void test_disabled_research_is_pending_not_a_local_alert_failure(void) {
    auto& ctx = resetGraph();
    constexpr bw::TimeMs kAfterGraceMs = bw::reliability::kSupervisionGraceMs + 1;
    for (const auto& row : kCoverage) {
        if (row.slot != TaskSlot::Research) {
            app::tasks::recordCycle(row.slot, kAfterGraceMs, 0, 0);
        }
    }
    const auto health = composeHealth(ctx, kAfterGraceMs);
    TEST_ASSERT_EQUAL(BeatState::Pending, f26::s_supervisor.state(TaskId::Research));
    TEST_ASSERT_TRUE(f26::s_supervisor.shouldFeed(kAfterGraceMs));
    TEST_ASSERT_BITS_LOW(faultBit(bw::FaultCode::TaskStalled), health.fault_bits);
}

void test_task_creation_failures_reach_real_registry_and_health(void) {
    for (const auto& row : kCoverage) {
        auto& ctx = resetGraph();
        app::tasks::reportTaskCreation(true, row.slot, ctx.faults, 0);
        TEST_ASSERT_BITS_LOW(faultBit(bw::FaultCode::TaskCreateFailed),
                             composeHealth(ctx, 0).fault_bits);
        app::tasks::reportTaskCreation(false, row.slot, ctx.faults, 1);
        TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::TaskCreateFailed),
                              composeHealth(ctx, 1).fault_bits);
    }
    app::tasks::reportTaskCreation(false, TaskSlot::Sensor, nullptr, 0);
}

void test_event_drop_reaches_real_registry_and_health(void) {
    auto& ctx = resetGraph();
    app::tasks::reportEventDrop(ctx.faults, 0);
    TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::EventPublishDrop),
                          composeHealth(ctx, 0).fault_bits);
    app::tasks::reportEventDrop(nullptr, 0);
}

void test_acquisition_tee_and_status_reach_diagnostics(void) {
    auto& ctx = resetGraph();
    f23::noteSensorStatuses(bw::SensorStatus::Ok, bw::SensorStatus::Failed);
    f26::noteSensorStatuses(bw::SensorStatus::Ok, bw::SensorStatus::Failed);
    bw::ImuSample imu{};
    imu.t_ms = 1;
    bw::PpgSample ppg{};
    ppg.t_ms = 1;
    f23::onImuSample(imu);
    f23::onPpgSample(ppg);
    const auto in = f23::gatherInput(ctx, 1);
    TEST_ASSERT_TRUE(in.has_imu_status);
    TEST_ASSERT_TRUE(in.has_ppg_status);
    TEST_ASSERT_EQUAL(bw::SensorStatus::Ok, in.imu_status);
    TEST_ASSERT_EQUAL(bw::SensorStatus::Failed, in.ppg_status);
    bw::SensorStatus published_imu{}, published_ppg{};
    TEST_ASSERT_TRUE(f23::latestSensorStatuses(published_imu, published_ppg));
    TEST_ASSERT_EQUAL(in.imu_status, published_imu);
    TEST_ASSERT_EQUAL(in.ppg_status, published_ppg);
    TEST_ASSERT_NOT_NULL(in.imu_raw);
    TEST_ASSERT_NOT_NULL(in.ppg_raw);
    TEST_ASSERT_EQUAL_UINT64(1, in.imu_raw[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(1, in.ppg_raw[0].t_ms);
    composeHealth(ctx, 1);
    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_BITS_HIGH(bw::reliability::facilityBit(bw::DegradedFacility::PpgLost),
                          state.degraded);
}

void test_watchdog_failure_remains_observable(void) {
    resetGraph();
    TEST_ASSERT_EQUAL(bw::Err::NotReady, f26::watchdogStatus());
    f26::noteWatchdogResult(bw::Err::HwFault);
    f26::noteWatchdogResult(bw::Err::Ok);
    TEST_ASSERT_EQUAL(bw::Err::HwFault, f26::watchdogStatus());
}

void test_real_held_ble_event_time_confirms_loss_and_recovers(void) {
    auto& ctx = resetGraph();
    bw::ble::LinkStateTracker tracker;
    bw::LatestValue<bw::BleLinkState> mailbox;
    bw::ble::LinkStateProvider provider{&mailbox};
    f26::attachBleLink(&provider);
    const auto publish = [&](bw::ble::LinkEvent::Kind kind, bw::TimeMs t) {
        tracker.apply({kind, bw::ble::kDefaultAttMtu, false, t});
        bw::BleLinkState state{};
        TEST_ASSERT_TRUE(tracker.snapshot(state));
        mailbox.set(state);
    };
    publish(bw::ble::LinkEvent::Kind::Up, 0);
    composeHealth(ctx, 0);
    publish(bw::ble::LinkEvent::Kind::Down, 1);
    composeHealth(ctx, 1);
    // NO periodic republish: the real F19 provider retains the event stamp.
    const bw::TimeMs deadline = 1 + bw::reliability::kBleDownConfirmMs;
    composeHealth(ctx, deadline - 1);
    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_BITS_LOW(bw::reliability::facilityBit(bw::DegradedFacility::BleDown),
                         state.degraded);
    const auto health = composeHealth(ctx, deadline);
    TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::BleDown), health.fault_bits);
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_BITS_HIGH(bw::reliability::facilityBit(bw::DegradedFacility::BleDown),
                          state.degraded);
    bw::BleLinkState held{};
    TEST_ASSERT_TRUE(provider.latest(held));
    TEST_ASSERT_EQUAL_UINT64(1, held.t_ms);
    publish(bw::ble::LinkEvent::Kind::Up, deadline + 1);
    composeHealth(ctx, deadline + 1);
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_BITS_LOW(bw::reliability::facilityBit(bw::DegradedFacility::BleDown),
                         state.degraded);
    // The diagnostic history stays sticky while current capability recovers.
    TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::BleDown), state.fault_bits);
    f26::attachBleLink(nullptr);
}

void test_boot_loop_is_published_without_disabling_local_alerts(void) {
    auto& ctx = resetGraph();
    bw::testkit::InMemoryKeyValueStore<> store;
    bw::reliability::CrashRecord previous{};
    previous.streak = bw::reliability::kMaxRebootsInWindow - 1;
    uint8_t bytes[bw::reliability::kCrashRecordBytes];
    const auto size = bw::reliability::encodeCrashRecord(previous, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL_UINT32(sizeof(bytes), size);
    TEST_ASSERT_TRUE(store.put(bw::reliability::kCrashRecordKey, bytes, size).ok());
    f26::noteBoot(bw::reliability::ResetReason::Panic, 0);
    f26::attachKeyValueStore(&store);  // same startup order as composition
    TEST_ASSERT_TRUE(f26::safeModeActive());
    const auto health = composeHealth(ctx, 0);
    TEST_ASSERT_BITS_HIGH(faultBit(bw::FaultCode::BootLoop), health.fault_bits);
    bw::ReliabilityState reliability{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(reliability));
    TEST_ASSERT_TRUE(reliability.safe_mode);
    TEST_ASSERT_BITS_HIGH(bw::reliability::facilityBit(bw::DegradedFacility::SafeMode),
                          reliability.degraded);

    // W6's approved detection/publication scope is deliberately NOT an F10
    // gate. With no phone/link/storage dependency, local alert demand still runs.
    bw::alert::AlertPolicy policy;
    bw::alert::PolicyInput input{};
    input.has_state = true;
    input.state.alertness = bw::AlertnessState::Drowsy;
    input.state.wear = bw::WearState::Worn;
    input.state.quality = {bw::DataQuality::Good, bw::DataQuality::Good};
    input.state.mode = bw::OperatingMode::Day;
    input.state.power = bw::PowerProfile::Normal;
    TEST_ASSERT_EQUAL(bw::AlertLevel::Warning, policy.evaluate(input).status.level);
    f26::attachKeyValueStore(nullptr);
}
