#pragma once

// firmware/src/app/wiring/wire_cataplexy.h — F09's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_cataplexy.* per F09 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

#include <cstdint>

#include <bw/cataplexy/candidate_detector.h>
#include <bw/cataplexy/event_window_requester.h>
#include <bw/providers.h>  // bw::IEventSink for attachEventSink (F17 slot)

namespace app {
struct AppContext;
}

namespace f09 {

// Constructs the feature's detector and window requester (default thresholds,
// F09 §8) — and, task table landed at P4 (2026-10-12, integrator), creates the
// 1 Hz pump that feeds MotionFeatures/PpgFeatures/StateSnapshot into the
// detector (the channel this function was waiting for; same deferral-then-land
// pattern as F01's SensorTask and F03's pump).
// Idempotence contract: call once, in the decision slot of the README's fixed
// call order (after F07's wire() — state_snapshot exists by then).
void wire(app::AppContext& ctx);

// Pump task body (runtime.md §3.2 StateTask row — F07/F08/F09 share the 1 s
// cadence, declared per wiring README): reads ctx.motion_features /
// ctx.ppg_features / ctx.state_snapshot, steps the detector once per NEW
// motion window (sequence change) and publishes a raised candidate as
// Event{SuspectedCataplexy} through the attached IEventSink — with F17 not
// landed the sink is null and the event is dropped and counted
// (droppedEvents()), never silently lost (F07 D11 precedent). PPG is passed
// only when co-tick with the motion window (equal sequence). Created by
// wire(); `arg` is the AppContext.
void pumpTask(void* arg);

// Event publishing (F09 §3): the F17 logger arrives in a later wave (W3).
// nullptr (the default) means a raised candidate is dropped and counted by
// the pump. The composition root calls this once F17 exists.
void attachEventSink(bw::IEventSink* sink);

// Raised-candidate events dropped because no sink is attached or a publish
// was rejected (pipeline health, H7 — f07::droppedEvents() precedent).
uint32_t droppedEvents();

// --- Pump constants (runtime.md §3.2; F09 spec §8) ---------------------------

// [DERIVED] One tick per F02 motion window (1000 ms): the drop rule judges
// over the last few 1 Hz windows, so the pump grid IS the feature cadence;
// the co-tick contract (same window for snapshot + features) holds on it.
constexpr uint32_t kCataplexyPumpPeriodMs = 1000;

// [EXPERIMENTAL] Stack bytes: held feature/snapshot frame (~150 B) plus
// FreeRTOS frames; 2 KB matches the other feature pumps. Re-measure with
// uxTaskGetStackHighWaterMark at Validate.
constexpr uint32_t kCataplexyPumpStackBytes = 2048;

// [DERIVED] Priority 3: W2 convention caps new core-1 pumps at <= 3; equal
// to the F07/F08 decision pumps it shares the cadence with.
constexpr uint32_t kCataplexyPumpPriority = 3;

// [runtime.md §3.2] Core 1 — same core as its F02/F07 producers, so the
// mailbox hand-off stays cache-local.
constexpr uint32_t kCataplexyPumpCore = 1;

// Composition-root accessors: both instances are private to wire_cataplexy.cpp;
// the integrator hands them to the pump. Consumers never include F09 headers —
// they receive the published Event through bw_core channels (01 §1 rule 3).
// Single 1 Hz pump: call detector().step() from one task only — the detector
// holds static history state and is not reentrant.
bw::cataplexy::EventCandidateDetector& detector();
bw::cataplexy::EventWindowRequester& windowRequester();

}  // namespace f09
