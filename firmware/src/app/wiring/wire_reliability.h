#pragma once

// firmware/src/app/wiring/wire_reliability.h — F26's wiring pair (convention:
// src/app/wiring/README.md; the file name wire_reliability.* is pinned by the
// F26 spec §2 Owns).
//
// Construction, dependency injection, the supervision table and the fault
// report entry point only — all behavior lives in bw_reliability (01 §1 rule
// 3), which the host suite drives with fake clocks. NOT behind
// `defined(ARDUINO)`: only the SupervisorTask body and the esp_*/f01 bindings
// are guarded (README §2 — this pair must compile for native too).
//
// See docs/features/F26_fault_handling_reliability.md (plan §16).

#include <cstdint>

#include <bw/providers.h>  // IEventSink / IKeyValueStore / ReliabilityState (CR-029/030)
#include <bw/reliability/boot_guard.h>           // ResetReason, BootGuard
#include <bw/reliability/reinit_scheduler.h>     // SensorKind, ReinitScheduler (D12)
#include <bw/reliability/watchdog_supervisor.h>  // TaskId for beat()

namespace app {
struct AppContext;
}

namespace f26 {

// --- task constants (plan D3, integrator-pinned at G1) --------------------

// [DERIVED] Why 1000 ms: G1 ruling 1 — runtime.md §3.2's row; a 1 s grid is
// 50x slower than the fastest supervised period (20 ms Sensor) and 1x the
// slowest (1 s pumps), so the scan cost is negligible and staleness
// detection still lands within one grid of a death.
constexpr uint32_t kSupervisorPeriodMs = 1000;

// [EXPERIMENTAL] Why 4096 B: G1 ruling 1 / §16.3 — runTick holds no large
// frames (a scan result + a PolicyInput ≈ 200 B) plus FreeRTOS/ESP-IDF
// frames. Re-measure with uxTaskGetStackHighWaterMark at DoD (Validate).
constexpr uint32_t kSupervisorStackBytes = 4096;

// [DERIVED] Why 3: G1 ruling 1 — ≤ 3 per the wiring convention, on par with
// F03's pump, above the 1–2 event-loop tasks so supervision runs promptly
// after a wake; it never blocks (pure scan + best-effort KV write).
constexpr uint32_t kSupervisorPriority = 3;

// [DERIVED] Why core 0: G1 ruling 1 — core 1 runs the sensor/processing
// pipeline; the supervisor + watchdog feed stays off it (runtime.md §3.2,
// BLE/storage convention on 0).
constexpr uint32_t kSupervisorCore = 0;

// --- seams -------------------------------------------------------------------

// Constructs the F26 graph (registry, policy, supervisor, boot guard,
// supervision table) and binds ctx.faults to the registry (D14). Idempotence
// contract: call once, BEFORE f01::wire so all reporters cache the registry.
// Sensor statuses arrive later through the SensorTask mailbox. On device this
// also calls noteBoot(). Attach all providers/store/sink before startSupervisor.
void wire(app::AppContext& ctx);

// Startup barrier: create the task only after every attach* call has completed.
// Avoids races on attachment pointers/BootGuard while core 0 is already ticking.
void startSupervisor(app::AppContext& ctx);

// SupervisorTask body (runtime.md §3.2): paces runTick on
// kSupervisorPeriodMs and owns the only esp_task_wdt feed in the system.
// Created by startSupervisor(); `arg` is the AppContext. Defined only under ARDUINO.
void supervisorTask(void* arg);

// --- runtime (called from the task / App::loop) ------------------------------

// One supervision cycle: heartbeat scan -> TaskStalled on enter edges (rows
// 5/6) -> degradation policy (rows 1-10) -> publish ReliabilityState ->
// rate-limited SensorFault events (D13) -> boot-loop exit -> WDT feed.
// Pure logic takes an explicit TimeMs (runtime.md §4) — host tests call it
// directly with a fake clock.
void runTick(bw::TimeMs now_ms);

// Freshness beat for a supervised task; guarded (beats arrive from other
// tasks, D8). F14 standalone mode marks everything idle — no beats, no scans.
void beat(bw::reliability::TaskId id, bw::TimeMs now_ms);

// SensorTask is the only runtime reader of driver status fields. Publish a
// value-copy here after acquisition; the core-0 supervisor reads the mailbox,
// never F01's mutable driver internals. Absent before the first publication.
void noteSensorStatuses(bw::SensorStatus imu, bw::SensorStatus ppg);

// Device init/add/feed outcomes: NotReady before setup, Ok after success,
// HwFault latched on failure for composition diagnostics (no matching FaultCode).
void noteWatchdogResult(bw::Err result);
bw::Err watchdogStatus();

// --- output seam (CR-030 mailbox) --------------------------------------------

// Copies the newest published ReliabilityState; false before the first
// runTick ("no value yet" — never fabricated, 00 §7).
bool latestReliabilityState(bw::ReliabilityState& out);

// Provider view of the same mailbox for F23-full (CR-030).
bw::IReliabilityStateProvider& stateProvider();

// D10 detection output: true when this boot entered safe mode (streak rule).
// Publication only — composition changes in safe mode are the P4
// integrator's call (G1 ruling 2).
bool safeModeActive();

// --- dependency injection (attach* survive wire(); noteBoot after wire()) ----

void attachEventSink(bw::IEventSink* sink);                      // D13 SensorFault mirror
void attachKeyValueStore(bw::IKeyValueStore* store);             // D10/D11 crash record
void attachStorageHealth(bw::IStorageHealthProvider* provider);  // row 3 input
void attachBleLink(bw::IBleLinkProvider* provider);              // row 4 input

// Captures the reset reason, reports row 8 (WatchdogReset/Brownout) and
// applies the persisted crash record (either order with attachKeyValueStore).
// On device called by wire(); host tests call it directly.
void noteBoot(bw::reliability::ResetReason reason, bw::TimeMs now_ms);

// CR-033(a) report site: latch FaultCode::TaskCreateFailed (severity Critical,
// kTaskCreateFailedSeverity) when xTaskCreate* != pdPASS — the task never
// started, which no heartbeat logic could ever detect. On device called by
// wire()'s ARDUINO branch (SupervisorTask); host tests call it directly (H10).
void reportTaskCreateFailure(bw::TimeMs now_ms);

// D12: the queued sensor-reinit ladder for a kind (sentinel -> IMU's ladder).
bw::reliability::ReinitScheduler& reinitScheduler(bw::reliability::SensorKind kind);

// SensorFault events the sink refused (counted AND reported as
// FaultCode::EventPublishDrop, CR-033 — see the report site in the .cpp).
uint32_t droppedEvents();

}  // namespace f26
