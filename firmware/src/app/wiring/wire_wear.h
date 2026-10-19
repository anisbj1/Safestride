#pragma once

#include <cstdint>

// firmware/src/app/wiring/wire_wear.h — F04's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_wear.* per F04 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).
// Unlike wire_hal this pair is NOT behind `defined(ARDUINO)`: WearDetector
// is pure logic and must compile for both esp32dev and native (README §3).

namespace app {
struct AppContext;
}

namespace bw::wear {
class WearDetector;
}

namespace f04 {

// Constructs the F04 graph and — task table, P4 (2026-10-12, integrator) —
// creates the 1 Hz wear pump, the channel this function was waiting for
// (same deferral-then-land pattern as F01's SensorTask and F03's pump).
// Idempotence contract: call once, in the "intelligence" slot of the
// README's fixed call order (after F02's wire() — motion_features exists).
void wire(app::AppContext& ctx);

// Pump task body (runtime.md §3.2 IntelligenceTask row, declared per wiring
// README): reads ctx.motion_features / ctx.ppg_features, steps the detector
// once per NEW motion window (sequence change — a stale mailbox never
// advances the dwell logic) and publishes ctx.wear_estimate for F07.
// PPG is passed only when its window is co-tick with the motion window
// (equal sequence); an absent/stalled PPG therefore reads as an absent
// source (nullptr) — the detector's honest IMU-only path. Created by
// wire(); `arg` is the AppContext.
void pumpTask(void* arg);

// --- Pump constants (runtime.md §3.2; F04 spec §8) ---------------------------

// [DERIVED] One tick per F02 motion window (1000 ms): wear evidence moves
// at feature cadence, so a faster tick would only re-step identical input;
// the 1 Hz grid matches F03's pump and the StateTask cadence it feeds.
constexpr uint32_t kWearPumpPeriodMs = 1000;

// [EXPERIMENTAL] Stack bytes: the live frame is WearInput (~60 B) plus
// FreeRTOS frames; 2 KB matches the other feature pumps with margin.
// Re-measure with uxTaskGetStackHighWaterMark at Validate.
constexpr uint32_t kWearPumpStackBytes = 2048;

// [DERIVED] Priority 3: W2 convention caps new core-1 pumps at <= 3; equal
// to F03's pump and to F07's consumer, one rung below ProcessingTask (4) —
// wear is a decision input, not the raw data path.
constexpr uint32_t kWearPumpPriority = 3;

// [runtime.md §3.2] Core 1 — same core as its F02 producers, so the
// mailbox hand-off stays cache-local.
constexpr uint32_t kWearPumpCore = 1;

// Composition-root accessor: the concrete detector lives in wire_wear.cpp;
// consumers reach it through wiring, never by including L5 headers
// (01 §1 rule 3).
bw::wear::WearDetector& detector();

}  // namespace f04
