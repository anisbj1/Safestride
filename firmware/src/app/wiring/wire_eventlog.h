#pragma once

// firmware/src/app/wiring/wire_eventlog.h — F17's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_eventlog.* per F17 spec §2
// Owns).
//
// Construction, dependency injection and task creation only, no behavior
// (01 §1 rule 3). This pair is NOT behind `defined(ARDUINO)`: the EventLogger
// is pure logic and must compile for both esp32dev and native (README §3);
// only the FreeRTOS task body is guarded.

#include <bw/eventlog/event_logger.h>
#include <bw/providers.h>

namespace app {
struct AppContext;
}

namespace f17 {

// Constructs the F17 graph: the EventLogger bound to AppContext's clock and
// fault sink, and publishes Event{Boot} (§15.2 D10 — exactly once per boot,
// idempotent on later calls). Also creates the event pump, at most once — a
// second wire() would otherwise spawn a drain task racing the first on the
// same queue. Idempotence
// contract: call once, FIRST among the F17 sequence of the composition root —
// after f01::wire (ctx.clock assigned) and BEFORE the attach* hooks below, per
// the integrator's P4 order (§15.5). Safe with an empty context (nullptr
// clock/faults: honest zeros, counters still record — §15.7).
void wire(app::AppContext& ctx);

// Event pump body (runtime.md §3.2; §15.2 D6): every kEventPumpPeriodMs it
// reads the monotonic clock and runs one drain cycle (append → fan-out, §10
// backoff on store failure). Created by wire(); `arg` is the AppContext.
void eventLogTask(void* arg);

// One drain cycle (the body eventLogTask paces): re-sync check, then
// append → fan-out until the queue is empty or the store refuses. Exposed so
// the native suite (README §3) drives the full wiring path without FreeRTOS;
// nothing above the composition root calls it.
void pumpOnce(bw::TimeMs now_ms);

// The logger itself: the composition root passes this pointer to the producer
// seams (f07/f08/f09::attachEventSink) and to f10::attachOutputs(..., sink)
// (§15.5). Never nullptr — constructed lazily on first use (F08 tracker()
// pattern), fully wired by wire().
bw::IEventSink* sink();

// F18's store (§15.2 D3): the first non-null attach runs the boot_id/seq
// recovery probe before draining starts; null-safe, order-independent for
// producers (queued events wait — §15.5).
void attachStore(bw::IEventStore* store);
// Composition-root fast path: hint 0 means known-empty; otherwise it is the
// mounted store's last persisted sequence. Invalid hints recover generically.
void attachStore(bw::IEventStore* store, uint32_t last_seq_hint);

// Fan-out list (§6 D1): consumers that want event-published notifications
// (F10/F14/F19/F12) attach here; false when kMaxEventSubscribers is reached.
bool attachSubscriber(bw::IEventSink* subscriber);

// §15.4 counters for diagnostics/F23 (F07 droppedEvents() pattern).
bw::eventlog::EventLogStats stats();

// Test seam (same status as pumpOnce): how many wire() calls have claimed the
// pump slot. Stays 1 forever on every platform, because the claim is evaluated
// before the ARDUINO guard that consumes it — so the native suite can assert
// wire()'s idempotence of the pump too (audit L-4), not just of the Boot.
uint32_t pumpClaims();

// --- Pump constants (wire_eventlog.h W2 convention; F17 spec §15.4) ----------

// [EXPERIMENTAL] Why 100 ms: bounds notification latency to one tick, and one
// drain empties the whole ≤ 16-slot queue (§11), so backlog age stays ~one
// period under a healthy store. Tune with F28/F29 data.
constexpr uint32_t kEventPumpPeriodMs = 100;

// [DERIVED] Why 2: runtime.md §3.2 "low" rung — below StateTask (3), so a
// late log pump can never delay a decision producer; within the wave budget
// for new pumps (priority ≤ 3). Max lateness is a diagnostic, not a failure.
constexpr uint32_t kEventPumpPriority = 2;

// [EXPERIMENTAL] Why 3072 B: the drain frame holds ~2 × Event (48 B) plus
// Result/FreeRTOS frames against ~500 B live data ≈ 3x margin (same reasoning
// as F07's state task). Re-measure with uxTaskGetStackHighWaterMark at
// Validate and adjust deliberately (§15.7).
constexpr uint32_t kEventPumpStackBytes = 3072;

// [DERIVED] Why 1: runtime.md §3.2 puts feature pumps on core 1 (same core as
// their producers) and the wave budget fixes new pumps to core 1 — see §15.2
// D6: the only flash-touching step is IEventStore::append, reached through
// F18's store contract, and every placement value here is a named constant so
// the integrator can relocate the pump without touching library code.
constexpr uint32_t kEventPumpCore = 1;

}  // namespace f17
