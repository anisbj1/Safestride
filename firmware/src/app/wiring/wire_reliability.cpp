#include "wire_reliability.h"
#include "task_health.h"

#include <atomic>

#include <bw/containers/latest_value.h>
#include <bw/platform_sync.h>
#include <bw/reliability/crash_record.h>
#include <bw/reliability/degradation_policy.h>
#include <bw/reliability/fault_registry.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace f26 {

namespace {

// esp_reset_reason() -> bw::reliability::ResetReason (boot_guard.h). Codes
// with no named reason map to Unknown — honest "unknown", never "clean"
// (00 §7). Generic/RTC WDT resets map to IntWatchdog: they are still WDT
// resets, so row 8 reports WatchdogReset for them too.
bw::reliability::ResetReason mapResetReason(const esp_reset_reason_t reason) {
    using bw::reliability::ResetReason;
    switch (reason) {
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
        return ResetReason::PowerOn;
    case ESP_RST_SW:
        return ResetReason::Software;
    case ESP_RST_PANIC:
        return ResetReason::Panic;
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        return ResetReason::IntWatchdog;
    case ESP_RST_TASK_WDT:
        return ResetReason::TaskWatchdog;
    case ESP_RST_BROWNOUT:
        return ResetReason::Brownout;
    case ESP_RST_DEEPSLEEP:
        return ResetReason::DeepSleep;
    default:
        return ResetReason::Unknown;
    }
}

}  // namespace

}  // namespace f26
#endif

namespace f26 {

namespace {

using bw::reliability::BootGuard;
using bw::reliability::CrashRecord;
using bw::reliability::DegradationPolicy;
using bw::reliability::FacilityEdge;
using bw::reliability::FaultRegistry;
using bw::reliability::PolicyInput;
using bw::reliability::ReinitScheduler;
using bw::reliability::ResetReason;
using bw::reliability::ScanResult;
using bw::reliability::SensorKind;
using bw::reliability::TaskConfig;
using bw::reliability::TaskId;
using bw::reliability::TaskTier;
using bw::reliability::WatchdogSupervisor;

// D3: FaultRegistry is pure and unlocked; THIS subclass is what the
// composition gets — reports arrive from several tasks, so every report()
// takes the bw_core mux (portMUX on device, no-op on host). Reads of the
// 32-bit fault word are guarded at the call sites in runTick().
class LockedFaultRegistry final : public FaultRegistry {
public:
    void report(bw::FaultCode code, bw::FaultSeverity severity, bw::TimeMs t_ms) override {
        const bw::CriticalSection guard;
        FaultRegistry::report(code, severity, t_ms);
    }
};

// --- supervision grids (spec §16.5, re-checked against the wire headers at
// IMPLEMENT 2026-10-18). Each cites its source constant; the stall deadline is
// kStallFactor x period (D7). [EXPERIMENTAL] grids.
constexpr uint32_t kSensorPeriodMs =
    20;  // wire_hal.h kSensorTaskPeriodMs = HW_SENSOR_PERIOD_MS (50 Hz IMU)
constexpr uint32_t kProcessingPeriodMs =
    1000;  // wire_f02.h kProcessingTaskPeriodMs = bw::signal::kMotionWindowMs
constexpr uint32_t kIntelligencePeriodMs =
    1000;  // wire_activity.h kActivityPumpPeriodMs (F03/F04/F06 pumps)
constexpr uint32_t kStatePeriodMs = 1000;  // wire_state.h kStateTaskPeriodMs
constexpr uint32_t kAlertPeriodMs = 50;    // bw::alert::kAlertTickPeriodMs (wire_alert.h, 20 Hz)
constexpr uint32_t kInputPeriodMs = 10;    // wire_input.h kInputTaskPeriodMs
constexpr uint32_t kDiagPeriodMs = 1000;   // wire_diag.h kDiagTaskPeriodMs
constexpr uint32_t kUiPeriodMs = 100;      // wire_ui.h kUiTaskPeriodMs = bw::ui::kUiFrameIntervalMs
constexpr uint32_t kStoragePeriodMs = 1000;  // wire_storage.h kStorageIdlePollMs (idle poll)
// [DERIVED] wire_ble.cpp paces at bw::ble::kCommsTickMs, including idle wakes.
constexpr uint32_t kCommsPeriodMs = 1000;
constexpr uint32_t kServicePumpPeriodMs =
    2000;  // runtime.md §3.2 (F20 pumpOnce grid; integrator's task)
// [DERIVED] wire_eventlog.h: bounded drain once per 100 ms.
constexpr uint32_t kEventLogPeriodMs = 100;
// [DERIVED] wire_baseline.h: one learning cycle per F02 window.
constexpr uint32_t kBaselinePeriodMs = 1000;
// [DERIVED] wire_research.h: 10 ms drain grid sustains capture throughput.
constexpr uint32_t kResearchPeriodMs = 10;

constexpr TaskConfig kSupervisedTasks[] = {
    {TaskId::Sensor, TaskTier::A, kSensorPeriodMs},
    {TaskId::Processing, TaskTier::A, kProcessingPeriodMs},
    {TaskId::Intelligence, TaskTier::A, kIntelligencePeriodMs},
    {TaskId::State, TaskTier::A, kStatePeriodMs},
    {TaskId::Alert, TaskTier::A, kAlertPeriodMs},
    {TaskId::Input, TaskTier::A, kInputPeriodMs},
    {TaskId::Diag, TaskTier::B, kDiagPeriodMs},
    {TaskId::Ui, TaskTier::B, kUiPeriodMs},
    {TaskId::Storage, TaskTier::B, kStoragePeriodMs},
    {TaskId::Comms, TaskTier::B, kCommsPeriodMs},
    {TaskId::ServicePump, TaskTier::B, kServicePumpPeriodMs},
    {TaskId::Wear, TaskTier::A, kIntelligencePeriodMs},
    {TaskId::Wake, TaskTier::A, kIntelligencePeriodMs},
    {TaskId::Sleep, TaskTier::A, kStatePeriodMs},
    {TaskId::Cataplexy, TaskTier::A, kStatePeriodMs},
    {TaskId::EventLog, TaskTier::B, kEventLogPeriodMs},
    {TaskId::Baseline, TaskTier::B, kBaselinePeriodMs},
    // A disabled research task never beats: pending Tier B is not a failure
    // and never gates immediate local alerting or watchdog feeding.
    {TaskId::Research, TaskTier::B, kResearchPeriodMs},
};

constexpr uint32_t kSupervisedTaskCount = sizeof(kSupervisedTasks) / sizeof(kSupervisedTasks[0]);

constexpr uint32_t tierMask(const TaskTier tier) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kSupervisedTaskCount; ++i) {
        if (kSupervisedTasks[i].tier == tier) {
            mask |= 1u << static_cast<uint8_t>(kSupervisedTasks[i].id);
        }
    }
    return mask;
}

constexpr uint32_t kTierAMask = tierMask(TaskTier::A);
constexpr uint32_t kTierBMask = tierMask(TaskTier::B);

TaskTier tierOf(const TaskId id) {
    for (uint32_t i = 0; i < kSupervisedTaskCount; ++i) {
        if (kSupervisedTasks[i].id == id) {
            return kSupervisedTasks[i].tier;
        }
    }
    return TaskTier::B;  // unreachable for the table's own ids
}

// --- static graph + seams (no heap, no init-order dependency) ---------------
LockedFaultRegistry s_registry;
DegradationPolicy s_policy;
WatchdogSupervisor s_supervisor;
BootGuard s_boot;
bw::LatestValue<bw::ReliabilityState> s_state;
struct SensorStatuses {
    bw::SensorStatus imu;
    bw::SensorStatus ppg;
};
bw::LatestValue<SensorStatuses> s_sensor_statuses;
ReinitScheduler s_reinit[static_cast<size_t>(SensorKind::Count)];

bw::IEventSink* s_sink = nullptr;
bw::IKeyValueStore* s_kv = nullptr;
bw::IStorageHealthProvider* s_storage = nullptr;
bw::IBleLinkProvider* s_ble = nullptr;

bool s_boot_noted = false;    // noteBoot() ran (device: inside wire())
bool s_boot_applied = false;  // crash record consumed exactly once
ResetReason s_reset_reason = ResetReason::Unknown;
bw::TimeMs s_boot_ms = 0;
bool s_task_created [[maybe_unused]] = false;  // device latch, deliberately NOT reset by wire()
uint32_t s_events_dropped = 0;
bool s_crash_write_fault_reported = false;
bool s_crash_write_pending = false;
bw::TimeMs s_crash_write_attempt_ms = 0;
bool s_crash_read_attempted = false;
bw::TimeMs s_crash_read_attempt_ms = 0;
std::atomic<bool> s_safe_mode{false};
std::atomic<bw::Err> s_watchdog_status{bw::Err::NotReady};
bool s_boot_event_emitted = false;  // D13(ii): one anomalous-boot event per boot
bool s_facility_event_sent[bw::reliability::kFacilityCount]{};
bw::TimeMs s_facility_event_ms[bw::reliability::kFacilityCount]{};

// --- crash record (D10/D11) -------------------------------------------------

// Best-effort persist: synchronous KV put in the caller's task (F18 §5 — the
// supervisor is not a real-time task). A failed put is handled, never
// ignored: it latches StorageDegraded once per boot (no dedicated code exists
// for "crash write failed", 19–24 are allocated — status-log CR request).
void writeCrashRecord(const bw::TimeMs now_ms) {
    s_crash_write_pending = true;
    if (s_kv == nullptr || !s_boot_applied) {
        return;
    }
    CrashRecord record{};
    record.streak = s_boot.streak();
    record.last_reset_reason = s_boot.reason();
    // boot_id: 0 = unknown — no F17 bootId() accessor at wiring yet
    // (wire_research.cpp:42 uses the same placeholder).  [ASSUMED]
    record.boot_id = 0;
    {
        const bw::CriticalSection guard;
        record.fault_bits_at_boot = s_registry.faultBits();
    }

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const size_t written = bw::reliability::encodeCrashRecord(record, buf, sizeof(buf));
    if (written == 0) {
        return;  // cannot happen with the fixed cap; encode() owns the check
    }
    const bw::Result<void> put = s_kv->put(bw::reliability::kCrashRecordKey, buf, written);
    s_crash_write_attempt_ms = now_ms;
    s_crash_write_pending = !put.ok();
    if (!put.ok() && !s_crash_write_fault_reported) {
        s_crash_write_fault_reported = true;
        s_registry.report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, now_ms);
    }
}

// Applies the persisted record once both inputs exist (noteBoot + store, in
// either order after wire()). Absent / corrupt / schema-mismatched records
// mean "no record" — streak base 0, normal boot (§16.8 test 3).
void applyBootRecord(const bw::TimeMs now_ms) {
    if (s_kv == nullptr || !s_boot_noted || s_boot_applied) {
        return;
    }
    uint8_t streak_base = 0;
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const bw::Result<size_t> got = s_kv->get(bw::reliability::kCrashRecordKey, buf, sizeof(buf));
    s_crash_read_attempted = true;
    s_crash_read_attempt_ms = now_ms;
    if (!got.ok() && got.err != bw::Err::NotFound && got.err != bw::Err::Corrupt &&
        got.err != bw::Err::Unsupported && got.err != bw::Err::Full) {
        // Busy/IO errors are not evidence of missing history. Never overwrite
        // an unread record with streak=1; retry on the supervisor's bounded grid.
        s_registry.report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, now_ms);
        return;
    }
    CrashRecord previous{};
    if (got.ok() && bw::reliability::decodeCrashRecord(buf, got.value, previous)) {
        streak_base = previous.streak;
    }
    s_boot.onBoot(streak_base, s_reset_reason);
    s_safe_mode.store(s_boot.safeMode(), std::memory_order_release);
    s_boot_applied = true;
    // Row 7: safe mode latches its code too (spec §16.4 — "SafeMode + code
    // BootLoop"), so ReliabilityState.fault_bits shows it, not only the flag.
    if (s_boot.safeMode()) {
        s_registry.report(bw::FaultCode::BootLoop, bw::FaultSeverity::Critical, s_boot_ms);
    }
    writeCrashRecord(now_ms);  // D10: written immediately once history is available
}

// --- SensorFault events (D13) -----------------------------------------------

bw::Event makeSensorFaultEvent(const bw::FaultCode code, const uint8_t facility_bit,
                               const bw::TimeMs now_ms) {
    bw::Event e{};
    e.t_start_ms = now_ms;  // producers never set seq/boot_id — F17 assigns them
    e.type = bw::EventType::SensorFault;
    e.detector_id = bw::reliability::kFaultDetectorId;  // 26, detector registry (D13)
    e.schema_version = bw::Event::kSchemaVersion;
    e.payload[0] = static_cast<uint8_t>(code);  // D13(i): triggering FaultCode
    e.payload[1] = facility_bit;                // D13(i): facility bit (mask); 0 = none
    return e;
}

// --- CR-033 report-site severities (fault.h: F00 supplies the vocabulary
// only, severity is policy picked where the failure is observed) ----------

// [DERIVED] Why Critical: the SupervisorTask owns the ONLY watchdog feed in
// the system (D9) and every Tier A gate. If its creation failed there is no
// supervision at all — nobody would ever feed the TWDT and no stall could be
// detected — the same system risk as a Tier A stall (§16.4 row 5, Critical),
// not merely a degraded service. Reported instead of the TaskStalled stand-in
// because the task NEVER STARTED: it never had a heartbeat, so the supervisor's
// beat logic could never have detected it (CR-033 alternative (a)).
constexpr bw::FaultSeverity kTaskCreateFailedSeverity = bw::FaultSeverity::Critical;

// [DERIVED] Why Warning: an event record was lost (data was consumed —
// core.md §2 Full-vs-Overflow rule), but every capability still functions:
// sensing and alerting are untouched, only the history record is thinner.
// Warning is fault.h's "still functioning" tier (LowBattery class); Error
// would claim a pipeline degraded and Critical would claim system risk,
// neither of which a lost record is. Severity choice pinned by CR-033
// ("a drop of an event is data loss => Warning").
constexpr bw::FaultSeverity kEventPublishDropSeverity = bw::FaultSeverity::Warning;

void publishSensorFault(const bw::Event& event) {
    if (s_sink == nullptr || !s_sink->publish(event).ok()) {
        // Result handled (counted AND reported), never ignored — F14
        // precedent; FaultCode::EventPublishDrop (CR-033) so the loss is
        // visible through ctx.faults / ReliabilityState.fault_bits (CR-030),
        // not only via droppedEvents().
        ++s_events_dropped;
        s_registry.report(bw::FaultCode::EventPublishDrop, kEventPublishDropSeverity,
                          event.t_start_ms);
    }
}

void emitFacilityEvent(const FacilityEdge& edge, const bw::TimeMs now_ms) {
    const uint8_t index = static_cast<uint8_t>(edge.facility);
    const bw::TimeMs last = s_facility_event_ms[index];
    if (s_facility_event_sent[index] && now_ms >= last &&
        now_ms - last < bw::reliability::kSensorFaultRepeatMs) {
        return;  // D13(i) rate limit: at most 1 event/min/facility while flapping
    }
    s_facility_event_sent[index] = true;
    s_facility_event_ms[index] = now_ms;
    publishSensorFault(makeSensorFaultEvent(
        edge.code, static_cast<uint8_t>(bw::reliability::facilityBit(edge.facility)), now_ms));
}

// D13(ii): exactly one event for an anomalous boot (reset reason that carries
// a WatchdogReset/Brownout code — row 8). Payload[1] = 0: a boot-level
// observation has no facility bit.
void maybeEmitBootEvent(const bw::TimeMs now_ms) {
    if (s_boot_event_emitted || !s_boot_noted) {
        return;
    }
    const bw::FaultCode code = BootGuard::resetFaultCode(s_reset_reason);
    if (code == bw::FaultCode::None) {
        return;  // ordinary boot: nothing to emit, ever
    }
    s_boot_event_emitted = true;
    publishSensorFault(makeSensorFaultEvent(code, 0, now_ms));
}

// --- tick pieces ------------------------------------------------------------

PolicyInput gatherInput(const bw::TimeMs now_ms, const uint32_t stalled_mask) {
    PolicyInput in{};
    in.t_ms = now_ms;
    {
        const bw::CriticalSection guard;  // report() writes this word from other tasks
        in.fault_bits = s_registry.faultBits();
    }
    in.safe_mode = s_boot.safeMode();
    SensorStatuses sensors{};
    if (s_sensor_statuses.get(sensors)) {
        in.has_imu_status = true;
        in.imu_status = sensors.imu;
        in.has_ppg_status = true;
        in.ppg_status = sensors.ppg;
    }
    if (s_storage != nullptr) {
        bw::StorageHealth health{};
        if (s_storage->latest(health)) {
            in.has_storage = true;
            in.storage = health;
        }
    }
    if (s_ble != nullptr) {
        bw::BleLinkState link{};
        if (s_ble->latest(link)) {
            in.has_ble = true;
            in.ble = link;
        }
    }
    in.tier_b_stalled_mask = stalled_mask & kTierBMask;  // Tier A is not a facility input (D7)
    return in;
}

}  // namespace

// --- public seams ------------------------------------------------------------

void runTick(const bw::TimeMs now_ms) {
    if (!s_boot_applied && s_boot_noted && s_kv != nullptr &&
        (!s_crash_read_attempted ||
         (now_ms >= s_crash_read_attempt_ms &&
          now_ms - s_crash_read_attempt_ms >= bw::reliability::kCrashWriteRetryMs))) {
        applyBootRecord(now_ms);
    }
    // 1. heartbeat scan — beats arrive from other tasks: hold the mux (D8).
    ScanResult scan{};
    {
        const bw::CriticalSection guard;
        scan = s_supervisor.scan(now_ms);
    }

    // 2. TaskStalled on enter edges (rows 5/6). Severity per tier: Tier A is
    //    Critical (watchdog path implicated, fault.h); Tier B is Error (a
    //    service is dead, F26 degrades to a reduced pipeline).
    bool tier_a_stall_entered = false;
    for (uint32_t i = 0; i < bw::reliability::kMaxSupervisedTasks; ++i) {
        const uint32_t bit = 1u << i;
        if ((scan.newly_stalled_mask & bit) == 0) {
            continue;
        }
        const TaskTier tier = tierOf(static_cast<TaskId>(i));
        s_registry.report(
            bw::FaultCode::TaskStalled,
            tier == TaskTier::A ? bw::FaultSeverity::Critical : bw::FaultSeverity::Error, now_ms);
        tier_a_stall_entered = tier_a_stall_entered || tier == TaskTier::A;
    }

    // 3. row 5: a Tier A stall precedes a reset — persist the record now
    //    (best-effort), so the next boot sees the latched bits.
    if (tier_a_stall_entered) {
        writeCrashRecord(now_ms);
    }

    // 4. degradation policy (rows 1–10).
    const bw::reliability::PolicyOutput out = s_policy.step(gatherInput(now_ms, scan.stalled_mask));

    // 5. policy-produced codes (rows 3/4 close the loop into the registry, D5).
    //    Severity: StorageDegraded = Error (reduced pipeline, fault.h);
    //    BleDown = Warning (nothing is lost, sync resumes on reconnect).
    for (uint8_t value = 0; value < bw::reliability::kMaxFaultCodes; ++value) {
        if ((out.codes_to_report & (1u << value)) == 0) {
            continue;
        }
        const bw::FaultCode code = static_cast<bw::FaultCode>(value);
        s_registry.report(code,
                          code == bw::FaultCode::BleDown ? bw::FaultSeverity::Warning
                                                         : bw::FaultSeverity::Error,
                          now_ms);
    }

    // 6. publish ReliabilityState (CR-030) — 1 Hz, false before this first set.
    bw::ReliabilityState state{};
    state.t_ms = now_ms;
    {
        const bw::CriticalSection guard;
        state.fault_bits = s_registry.faultBits();
    }
    state.degraded = out.degraded;
    state.safe_mode = s_boot.safeMode();
    s_state.set(state);

    // 7. facility ENTER edges -> rate-limited SensorFault events (D13(i)).
    for (uint8_t i = 0; i < out.edge_count; ++i) {
        emitFacilityEvent(out.edges[i], now_ms);
    }

    // 8. anomalous boot -> one SensorFault (D13(ii)).
    maybeEmitBootEvent(now_ms);

    // 9. D10 exit: a boot that survived kBootLoopBootLimitMs clears the streak
    //    (persisted immediately; safe_mode itself only changes on next boot).
    if (s_boot.tickSurvived(now_ms, s_boot_ms)) {
        writeCrashRecord(now_ms);
    }

    // D17: persist the latest state, not an obsolete failed-write image.
    // Never retry before boot history was loaded; no repeated writes after success.
    if (s_crash_write_pending && s_boot_applied && s_kv != nullptr &&
        now_ms >= s_crash_write_attempt_ms &&
        now_ms - s_crash_write_attempt_ms >= bw::reliability::kCrashWriteRetryMs) {
        writeCrashRecord(now_ms);
    }

#if defined(ARDUINO)
    // 10. D9: the ONLY watchdog feed in the system, and only when every Tier A
    //     heartbeat is fresh. If this loop dies, nobody feeds -> reset (§10).
    if (s_supervisor.shouldFeed(now_ms)) {
        noteWatchdogResult(esp_task_wdt_reset() == ESP_OK ? bw::Err::Ok : bw::Err::HwFault);
    }
#endif
}

void beat(const TaskId id, const bw::TimeMs now_ms) {
    const bw::CriticalSection guard;  // beats come from other tasks (D8)
    s_supervisor.beat(id, now_ms);
}

void noteSensorStatuses(const bw::SensorStatus imu, const bw::SensorStatus ppg) {
    s_sensor_statuses.set({imu, ppg});
}

void noteWatchdogResult(const bw::Err result) {
    // Platform errors stay visible even if a later feed succeeds. No existing
    // FaultCode means watchdog setup/feed failure; do not invent a reset event.
    if (result != bw::Err::Ok || s_watchdog_status.load() != bw::Err::HwFault) {
        s_watchdog_status.store(result);
    }
}

bw::Err watchdogStatus() {
    return s_watchdog_status.load();
}

bool latestReliabilityState(bw::ReliabilityState& out) {
    return s_state.get(out);
}

namespace {

// CR-030 attach point for F23-full: one provider view of the same mailbox.
class ReliabilityStateProvider final : public bw::IReliabilityStateProvider {
public:
    bool latest(bw::ReliabilityState& out) const override { return s_state.get(out); }
};

ReliabilityStateProvider s_state_provider;

}  // namespace

bw::IReliabilityStateProvider& stateProvider() {
    return s_state_provider;
}

bool safeModeActive() {
    return s_safe_mode.load(std::memory_order_acquire);
}

void attachEventSink(bw::IEventSink* sink) {
    s_sink = sink;
}

void attachKeyValueStore(bw::IKeyValueStore* store) {
    s_kv = store;
    applyBootRecord(s_boot_ms);  // startup-only; no-op until noteBoot() ran
}

void attachStorageHealth(bw::IStorageHealthProvider* provider) {
    s_storage = provider;
}

void attachBleLink(bw::IBleLinkProvider* provider) {
    s_ble = provider;
}

void noteBoot(const ResetReason reason, const bw::TimeMs now_ms) {
    s_reset_reason = reason;
    s_boot_noted = true;
    s_boot_ms = now_ms;
    const bw::FaultCode code = BootGuard::resetFaultCode(reason);
    if (code != bw::FaultCode::None) {
        s_registry.report(code, bw::FaultSeverity::Critical, now_ms);  // row 8
    }
    applyBootRecord(now_ms);  // no-op until the store is attached
}

ReinitScheduler& reinitScheduler(const SensorKind kind) {
    size_t index = static_cast<size_t>(kind);
    if (index >= static_cast<size_t>(SensorKind::Count)) {
        index = 0;  // caller passed a sentinel: IMU's ladder is the safer default
    }
    return s_reinit[index];
}

uint32_t droppedEvents() {
    return s_events_dropped;
}

// CR-033(a) report site: xTaskCreate* returned != pdPASS for a task F26 owns
// (SupervisorTask), so no supervision runs this boot. Kept OUT of the ARDUINO
// guard so the host suite can drive the site directly (H10); on device it is
// called by wire()'s creation branch only.
void reportTaskCreateFailure(const bw::TimeMs now_ms) {
    s_registry.report(bw::FaultCode::TaskCreateFailed, kTaskCreateFailedSeverity, now_ms);
}

// --- construction + task -----------------------------------------------------

void wire(app::AppContext& ctx) {
    // Fresh shell: the whole graph and boot state reset, so a second wire()
    // behaves like a reboot. The attach* pointers deliberately SURVIVE (they
    // are not listed here) — see the header's call-order contract.
    s_registry = LockedFaultRegistry{};
    s_policy = DegradationPolicy{};
    s_supervisor = WatchdogSupervisor{};
    s_boot = BootGuard{};
    s_state = bw::LatestValue<bw::ReliabilityState>{};
    s_sensor_statuses = bw::LatestValue<SensorStatuses>{};
    for (ReinitScheduler& scheduler : s_reinit) {
        scheduler = ReinitScheduler{};
    }
    s_boot_noted = false;
    s_boot_applied = false;
    s_reset_reason = ResetReason::Unknown;
    s_boot_ms = 0;
    s_events_dropped = 0;
    s_crash_write_fault_reported = false;
    s_crash_write_pending = false;
    s_crash_write_attempt_ms = 0;
    s_crash_read_attempted = false;
    s_crash_read_attempt_ms = 0;
    s_safe_mode.store(false, std::memory_order_release);
    s_watchdog_status.store(bw::Err::NotReady);
    s_boot_event_emitted = false;
    for (bool& sent : s_facility_event_sent) {
        sent = false;
    }
    for (bw::TimeMs& stamp : s_facility_event_ms) {
        stamp = 0;
    }
    for (uint32_t i = 0; i < kSupervisedTaskCount; ++i) {
        s_supervisor.configure(kSupervisedTasks[i]);
    }

    // D14: the composition reports into the registry from here on (01 §7).
    ctx.faults = &s_registry;

#if defined(ARDUINO)
    noteBoot(mapResetReason(esp_reset_reason()), ctx.clock != nullptr ? ctx.clock->nowMs() : 0);
#endif
}

void startSupervisor(app::AppContext& ctx) {
#if defined(ARDUINO)
    if (!s_task_created) {
        // CR-033(a): a SupervisorTask that never started is reported with its
        // OWN code (TaskCreateFailed), not the TaskStalled stand-in — a task
        // without a heartbeat is invisible to the supervisor's beat logic, so
        // the two symptoms must not be conflated. Severity: kTaskCreateFailedSeverity.
        if (xTaskCreatePinnedToCore(supervisorTask, "F26Sup", kSupervisorStackBytes, &ctx,
                                    kSupervisorPriority, nullptr, kSupervisorCore) == pdPASS) {
            s_task_created = true;
        } else {
            reportTaskCreateFailure(ctx.clock != nullptr ? ctx.clock->nowMs() : 0);
        }
    }
#else
    (void)ctx;
#endif
}

#if defined(ARDUINO)

void supervisorTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // D9: the supervisor subscribes ONLY ITSELF to the TWDT (re-init and
    // re-subscribe are tolerated: a boot where the core already runs the WDT
    // keeps the default timeout, which equals kTaskWatchdogTimeoutS).
    noteWatchdogResult(esp_task_wdt_init(bw::reliability::kTaskWatchdogTimeoutS, true) == ESP_OK
                           ? bw::Err::Ok
                           : bw::Err::HwFault);
    noteWatchdogResult(esp_task_wdt_add(nullptr) == ESP_OK ? bw::Err::Ok : bw::Err::HwFault);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil): a late tick shifts only itself.
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // Supervision must never stop: ctx.clock when the composition has
        // one, else the platform tick (monotonic ms since boot) — pure logic
        // still receives an explicit TimeMs argument either way.
        const bw::TimeMs now =
            (ctx.clock != nullptr)
                ? ctx.clock->nowMs()
                : static_cast<bw::TimeMs>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
        runTick(now);
        app::tasks::completeCycle(bw::diag::TaskSlot::Supervisor, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSupervisorPeriodMs));
    }
}

#endif  // ARDUINO

}  // namespace f26
