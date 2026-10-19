#pragma once

// firmware/src/app/wiring/wire_state.h — F07's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_state.* per F07 spec §2
// Owns).
//
// Construction, dependency injection and task creation only, no behavior
// (01 §1 rule 3). This pair is NOT behind `defined(ARDUINO)`: StateEngine is
// pure logic and must compile for both esp32dev and native (README §3);
// only the FreeRTOS task body is guarded.

#include <bw/containers/latest_value.h>
#include <bw/context.h>
#include <bw/mode_profile.h>
#include <bw/model.h>
#include <bw/providers.h>
#include <bw/state/state_engine.h>

namespace app {
struct AppContext;
}

namespace f07 {

// Constructs the F07 graph: the StateEngine with the §8 default thresholds
// and the StateProvider, and — task table (F07 §15) — creates StateTask,
// which steps the engine at 1 Hz from the AppContext channels and publishes
// the snapshot. Idempotence contract: call once, in the "decision" slot of
// the README's fixed call order (after f01::wire — ctx.clock is assigned by
// then; the engine itself is constructed at file scope, order-free).
void wire(app::AppContext& ctx);

// StateTask body (runtime.md §3.2, declared per wiring README): the FreeRTOS
// shell around bw::state::StatePump (spec D15) — it reads the clock and paces
// the 1 Hz ticks; the pump does the sequence-gated mailbox pulls, derives
// SourceQuality from motion_features/ppg_features with staleness aging
// (missing/stale => Insufficient), steps the engine with time as data (D4)
// and publishes the snapshot plus transition records. Created by wire();
// `arg` is the AppContext.
void stateTask(void* arg);

// --- W1 mailbox binding (P4, the integrator) -------------------------------
// app.h grows the three W1 fields (wear_estimate, alertness_probabilities,
// state_snapshot — pre-agreed names) together with the f07::wire(ctx) call,
// and app.cpp then makes ONE more call:
//     f07::attachMailboxes(&ctx.wear_estimate, &ctx.alertness_probabilities,
//                          &ctx.state_snapshot);
// Until that call these ports stay nullptr and the pump treats the axes as
// never-received: the engine gates to Unknown + SensorUnavailable (honest,
// never a fabricated Awake) and no snapshot is published. Wiring-owned, so
// the field names are not a compile-time dependency of this file (like
// f10::attachOutputs — wiring carries its own dependency injection).
void attachMailboxes(const bw::LatestValue<bw::WearEstimate>* wear,
                     const bw::LatestValue<bw::AlertnessProbabilities>* alertness,
                     bw::LatestValue<bw::StateSnapshot>* snapshot);

// Event publishing (D11): the F17 logger arrives in a later wave. nullptr
// (the default) means transition records are COUNTED as dropped instead of
// blocking the pump; the sink may be swapped at runtime by the integrator.
void attachEventSink(bw::IEventSink* sink);

// --- P4b quality-provider seam (spec D16, Wave 4) ---------------------------
// ONE composition-root call, beside attachMailboxes — the integrator's P4
// app.cpp line (F07 never includes F23; 01 §1 rule 1):
//     f07::attachQualityProvider(f23::latestSourceQuality);
// Until that call the port stays nullptr and the pump keeps deriving
// SourceQuality from the F02 feature mailboxes byte-for-byte (missing/stale
// => Insufficient, auditor A-03 — regression-critical invariant, §9 pins
// it). Once bound, F23's value FULLY substitutes for the derivation: a
// `true` return writes both quality channels + stamps the receipt;
// `false`/silence ages the channels back to Insufficient through the same
// kFeatureQualityStaleMs machinery (no new constant, no retune — D16).
// Plain function pointer: no F23 include in this file, no heap, no
// std::function. "Absence" is not knowing: never called ⇒ honest derivation.
void attachQualityProvider(bool (*provider)(bw::SourceQuality& out));

// Startup-only binding, before wire() creates StateTask (W6 P4).
void attachModeProvider(const bw::IModeProvider* provider);

// --- Pump constants (runtime.md §3.2; F07 spec §8) -------------------------

// [DERIVED] One tick per feature window: F02 publishes Motion/PpgFeatures
// exactly once per 1000 ms window and the pump starts after F02/F03 (fixed
// wire order), so a 1 s grid observes every publish at a constant phase —
// identical to F03's pump. The 30 s alertness epoch only needs a fresh read
// within kAlertnessStaleMs (60 s), which the sequence gate guarantees.
constexpr uint32_t kStateTaskPeriodMs = 1000;

// [EXPERIMENTAL] Stack bytes: the frame holds StateInputs + StateOutputs
// (~500 B together — the Event is ~50 B, 02 §4.5) plus five pulled contract
// values and FreeRTOS frames; 3 KB is ~3x the live data. Re-measure with
// uxTaskGetStackHighWaterMark at DoD (Validate stage).
constexpr uint32_t kStateTaskStackBytes = 3072;

// [DERIVED] Priority 3: the decision consumer sits at the IntelligenceTask
// "med" rung (runtime §3.2), equal to F03's pump and one rung below F02's
// ProcessingTask (4) — a consumer never outranks the data path it reads.
constexpr uint32_t kStateTaskPriority = 3;

// [runtime.md §3.2] Core 1 — same core as its producers (F02/F03), so the
// mailbox hand-off stays cache-local.
constexpr uint32_t kStateTaskCore = 1;

// Composition-root accessors: the engine/provider live in wire_state.cpp;
// tests, diagnostics and future wiring reach them here (01 §1 rule 3).
// Consumers never include F07 headers — they read StateSnapshot through
// bw_core (IStateProvider / the state_snapshot mailbox).
bw::state::StateEngine& engine();
bw::state::StateProvider& provider();

// Pipeline health (spec §14 health card H7): when the engine last stepped,
// the worst observed wake lateness of the task, and transition records
// dropped because no sink is attached or publish() was rejected (D11: the
// pump never blocks on F17).
bw::TimeMs lastRunMs();
uint32_t maxLatenessMs();
uint32_t droppedEvents();

}  // namespace f07
