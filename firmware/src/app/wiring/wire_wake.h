#pragma once

// firmware/src/app/wiring/wire_wake.h — F06's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_wake.* per F06 spec §2 Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

#include <cstdint>

#include <bw/wake/wakefulness_estimator.h>

namespace app {
struct AppContext;
}

namespace f06 {

// Constructs F06's estimator and injects what AppContext carries today.
// Idempotence contract: call once, in the "intelligence" slot of the
// README's fixed call order (after F01's wire(), which assigns the device
// clock — app.h — and after F02's wire()).
//
// Dependencies that do not exist yet are passed as nullptr and are handled
// by the estimator itself (F06 §3):
//  - baseline: F13's IBaselineProvider is not in AppContext yet, so the
//    z-score HR term stays off and confidence carries the penalty;
//  - model: F05's IModel arrives with M5; the heuristic scorer answers
//    until then, which is exactly F06 D1's plan.
//
// No task was created at feature time: the 1 Hz pump that feeds WakeSample
// is owned by the integrator's task table (same deferral pattern as F01's
// SensorTask and F03's classifier - see those status logs) and landed at
// P4 (2026-10-12) now that AppContext carries the channels. Pump contract:
// build WakeSample from features stamped at the SAME tick (the assembler
// does not compare feature t_ms - freshness is the pump's responsibility),
// default-construct nothing (a zeroed MotionFeatures reads as Good + 0 g =
// fabricated low-motion evidence), and forward valid=false estimates as an
// output, not silence; at least one step per 30 s epoch.
void wire(app::AppContext& ctx, const bw::IBaselineProvider* baseline = nullptr);

// Pump task body (runtime.md §3.2 IntelligenceTask row, declared per wiring
// README): reads ctx.motion_features / ctx.ppg_features / ctx.activity_estimate,
// steps the estimator once per NEW motion window and publishes
// ctx.alertness_probabilities for F07 — only when a NEW epoch answer appears
// (probs.t_ms names the epoch), so the LatestValue overwrite counter stays a
// real data-loss signal (F00 D10). Created by wire(); `arg` is the AppContext.
void pumpTask(void* arg);

// --- Pump constants (runtime.md §3.2; F06 spec §8) ---------------------------

// [DERIVED] One tick per F02 motion window (1000 ms): the assembler folds
// one WakeSample per window and decides per 30 s epoch, so the pump grid IS
// the feature cadence; faster polling would duplicate windows.
constexpr uint32_t kWakePumpPeriodMs = 1000;

// [EXPERIMENTAL] Estimation/epoch work plus integer-only serial diagnostics.
// The former 2 KB budget triggered F06Pump's stack canary on the device after
// telemetry was added. Reserve 4 KB headroom; this remains an estimate until
// uxTaskGetStackHighWaterMark is checked on the corrected hardware build.
constexpr uint32_t kWakePumpStackBytes = 4096;

// [DERIVED] Priority 3: W2 convention caps new core-1 pumps at <= 3; equal
// to F03's pump and to F07's consumer, below the F02 data path (4).
constexpr uint32_t kWakePumpPriority = 3;

// [runtime.md §3.2] Core 1 — same core as its F02/F03 producers, so the
// mailbox hand-off stays cache-local.
constexpr uint32_t kWakePumpCore = 1;

// Composition-root accessor: the estimator instance is private to
// wire_wake.cpp; the integrator hands it to the pump that feeds 1 s windows
// and drains the epoch estimate for F07's mailbox (01 §1 rule 3). Consumers
// never include F06 headers — they receive AlertnessProbabilities through
// bw_core channels. Valid after wire().
bw::wake::WakefulnessEstimator& estimator();

}  // namespace f06
