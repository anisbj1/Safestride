#include "wire_activity.h"
#include "task_health.h"

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the pump (device build only)
#endif

namespace f03 {

namespace {

// Static-storage classifier (no heap: construction happens before any task
// runs, embedded conventions). Threshold defaults live in bw::activity's
// named constants (F03 §8).
bw::activity::ActivityClassifier s_classifier{};

}  // namespace

#if defined(ARDUINO)

void pumpTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil) — a late tick shifts only itself, keeping the pump in
    // constant phase with F02's 1 Hz publishes.
    TickType_t last_wake = xTaskGetTickCount();

    // Sequence of the last window we stepped. LatestValue::sequence() starts
    // at 0 and increments per set(), so 0 = "nothing published yet".
    uint32_t seen_seq = 0;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        const uint32_t seq = ctx.motion_features.sequence();
        if (seq != seen_seq) {
            // Comparing by difference stays correct across a 2^32 wrap
            // (latest_value.h); a jump of 2+ means windows were overwritten
            // unread — stepping with the newest one makes the classifier see
            // the t_ms gap and reset its dwell (F03 D4: evidence not
            // continuous), which is the honest response to lost windows.
            seen_seq = seq;
            bw::MotionFeatures features{};
            if (ctx.motion_features.get(features)) {
                ctx.activity_estimate.set(s_classifier.step(features));
            }
        }
        // No new window: do NOT re-step — repeating identical features would
        // advance the dwell logic on evidence that never arrived.

        app::tasks::completeCycle(bw::diag::TaskSlot::Intelligence, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kActivityPumpPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction only - s_classifier is already constructed with default
    // thresholds. Task table landed 2026-10-08: wire() also creates the 1 Hz
    // pump, which reads AppContext's motion_features mailbox (integrator,
    // same day) and publishes activity_estimate. The pump may run before F02
    // publishes anything: sequence() stays 0, the loop just waits — an empty
    // channel is an ordinary state, never a stall or a fabricated estimate.
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(pumpTask, "F03Pump", kActivityPumpStackBytes,
                                                     &ctx, kActivityPumpPriority, nullptr,
                                                     kActivityPumpCore),
                             bw::diag::TaskSlot::Intelligence, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::activity::ActivityClassifier& classifier() {
    return s_classifier;
}

}  // namespace f03
