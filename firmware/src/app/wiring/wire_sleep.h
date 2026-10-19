#pragma once

// firmware/src/app/wiring/wire_sleep.h — F08's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_sleep.* per F08 spec §2
// Owns).
//
// Construction, dependency injection and task creation only, no behavior
// (01 §1 rule 3). This pair is NOT behind `defined(ARDUINO)`: the tracker is
// pure logic and must compile for both esp32dev and native (README §3);
// only the FreeRTOS task body is guarded.

#include <bw/containers/latest_value.h>
#include <bw/providers.h>
#include <bw/sleep/sleep_episode_tracker.h>
#include <bw/state.h>

namespace app {
struct AppContext;
}

namespace f08 {

// Constructs the F08 graph: the SleepEpisodeTracker with the §8 default
// constants, bound to AppContext.state_snapshot through an IStateProvider
// adapter (bw_core only — F08 never includes F07 headers, 01 §1 rule 1).
// Also runs recover() exactly once (§6 D4: before the first step()) and
// creates the episode pump. Idempotence contract: call once, in the
// "decision" slot of the README's fixed call order (after f07::wire — the
// state_snapshot mailbox exists by then; before F10, per README).
void wire(app::AppContext& ctx);

// Episode pump body (runtime.md §3.2 StateTask row — F07/F08/F09 share the
// 1 s cadence): polls IStateProvider::latest() every kSleepPollPeriodMs, steps
// the tracker, publishes has_event through IEventSink and writes the
// EpisodeStatus mailbox only on status_changed (setting it every tick would
// inflate LatestValue::overwrites(), a real data-loss counter — F00 D10, F08
// §7). Created by wire(); `arg` is the AppContext.
void sleepTask(void* arg);

// One poll → step → deliver cycle (the body sleepTask paces): reads
// IStateProvider::latest(), steps the tracker, delivers the result — events
// while detached go to the pending FIFO (see attachEventSink). Exposed so the
// native suite (README §3) drives the full wiring path without FreeRTOS;
// nothing above the composition root calls it.
void pumpOnce();

// Event publishing (F08 §3): the F17 logger arrives in a later wave. nullptr
// (the default) means episodes are buffered until a sink is attached, then
// never dropped — delivery is INDEPENDENT of attach order (audit M-1): wire()
// may run recover() and the pump before this hook runs; the produced events
// wait in a small bounded FIFO (drop-oldest, §10 never blocks the machine)
// and are flushed in order on the first non-null attach. A publish rejected
// during the flush is dropped, not retried — §10; F17's counters own the drop.
void attachEventSink(bw::IEventSink* sink);

// Live episode status for F11/F12 (F08 §3, CR-007): the app-owned mailbox
// instance lives at the attach site (wire_state.h pattern); nullptr until the
// integrator binds it at P4.
void attachEpisodeStatus(bw::LatestValue<bw::EpisodeStatus>* status);

// --- Pump constants (runtime.md §3.2; F08 spec §8) ---------------------------

// [DERIVED] Why: pump period = the 1 s StateTask cadence of runtime.md §3.2
// (core 1, med, owner F07/F08/F09) — IStateProvider::latest() advances at that
// rate, so faster polling only trips the row-1 de-duplication no-op while
// wasting wake-ups.
constexpr uint32_t kSleepPollPeriodMs = 1000;

// [DERIVED] Why: W2 convention caps new core-1 pumps at priority ≤ 3; 2 sits
// below the ceiling so a late episode pump can never delay a higher-numbered
// neighbour — episodes are second-granularity, max lateness is a diagnostic
// (runtime.md §3.2 rules), not a failure.
constexpr uint32_t kSleepPumpPriority = 2;

// [EXPERIMENTAL] Why: the pump only polls, steps and publishes (no buffers, no
// formatting); 2048 B ≈ 4x a StepOutput frame (Event 48 B + status) plus
// FreeRTOS overhead. Re-measure with uxTaskGetStackHighWaterMark at Validate
// and adjust deliberately (F08 §11).
constexpr uint32_t kSleepPumpStackBytes = 2048;

// [DERIVED] Why: core 1 — same core as its producer (F07's StateTask,
// runtime.md §3.2), so the mailbox hand-off stays cache-local.
constexpr uint32_t kSleepPumpCore = 1;

// Composition-root accessors: the tracker lives in wire_sleep.cpp; tests,
// diagnostics and future wiring reach it here (01 §1 rule 3). Consumers never
// include bw_sleep headers — events travel via IEventSink, status via the
// app-owned LatestValue<EpisodeStatus>.
bw::sleep::SleepEpisodeTracker& tracker();

}  // namespace f08
