#pragma once

// firmware/src/app/wiring/wire_activity.h — F03's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_activity.* per F03 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

#include <bw/activity/activity_classifier.h>

namespace app {
struct AppContext;
}

namespace f03 {

// Constructs the feature's classifier (default thresholds).
// Idempotence contract: call once, in the intelligence slot of the README's
// fixed call order (after F02's wire()).
//
// Task table landed 2026-10-08: wire() also creates the 1 Hz pump (see
// pumpTask below) now that AppContext carries the MotionFeatures mailbox
// (integrator, same day) - the channel this function was waiting for.
void wire(app::AppContext& ctx);

// Pump task body (runtime.md §3.2 IntelligenceTask row "1 s (features)",
// declared per wiring README): reads ctx.motion_features, steps the
// classifier once per NEW window (sequence change - a stale mailbox never
// advances the dwell logic), publishes ctx.activity_estimate. Created by
// wire(); `arg` is the AppContext.
void pumpTask(void* arg);

// --- Pump constants (runtime.md §3.2: F03 sits in the IntelligenceTask row) --
// Instantiated per owner 2026-10-08 (status log): F03 creates its OWN task
// rather than one task shared with F04/F06, because a shared task body would
// put F04/F06 code inside F03's wiring file (ownership rule, wiring README).
// If the shared IntelligenceTask is wanted later, the integrator moves these
// constants - a mechanical refactor, no behavior change.

// [DERIVED] One tick per F02 feature window: F02 publishes exactly once per
// kMotionWindowMs (1000 ms - bw/signal/signal_config.h; deliberately NOT
// included here, bw_core-only rule, so the value is documented, not
// referenced), and the pump starts after F02 (fixed wire order), so a 1 s
// tick observes every window exactly once at a constant phase offset.
// Matches runtime.md §3.2's "1 s (features)".
constexpr uint32_t kActivityPumpPeriodMs = 1000;

// [EXPERIMENTAL] Stack bytes: classifier state and decision tables are
// static/const; the live frame is MotionFeatures (~40 B) + ActivityEstimate
// + FreeRTOS frames, so 2 KB matches SensorTask/ProcessingTask with margin.
// Re-measure at DoD with uxTaskGetStackHighWaterMark.
constexpr uint32_t kActivityPumpStackBytes = 2048;

// [EXPERIMENTAL] Priority 3: runtime.md §3.2 ranks IntelligenceTask "med",
// one rung below ProcessingTask (4) which is below SensorTask (5) - the
// high > med-high > med data-path ladder. Tune when the full table lands.
constexpr uint32_t kActivityPumpPriority = 3;

// [runtime.md §3.2] Core 1 - same core as its producer (F02), so the mailbox
// hand-off stays cache-local.
constexpr uint32_t kActivityPumpCore = 1;

// Composition-root accessor: the classifier instance is private to
// wire_activity.cpp; tests and diagnostics reach it here (01 §1 rule 3).
// Consumers never include F03 headers — they receive ActivityEstimate
// through bw_core channels (AppContext's activity_estimate mailbox).
bw::activity::ActivityClassifier& classifier();

}  // namespace f03
