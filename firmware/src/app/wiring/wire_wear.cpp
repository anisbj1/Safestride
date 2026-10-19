#include "wire_wear.h"
#include "task_health.h"

#include <bw/wear/wear_detector.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the pump (device build only)
#endif

namespace f04 {
namespace {

// File-scope, default WearConfig: static storage, no heap, no init-order
// dependency (F04 injects nothing at construction time yet).
bw::wear::WearDetector g_detector{};

}  // namespace

#if defined(ARDUINO)

void pumpTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil) — a late tick shifts only itself, keeping the pump
    // in constant phase with F02's 1 Hz window publishes.
    TickType_t last_wake = xTaskGetTickCount();

    // Motion sequence of the last window we stepped; 0 = nothing published
    // yet (LatestValue::sequence() starts at 0 and increments per set()).
    uint32_t seen_motion_seq = 0;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // No clock yet: honest wait (F02/F07 rule) — the dwell logic needs
        // real time, never a fabricated t = 0.
        if (ctx.clock != nullptr) {
            const uint32_t seq = ctx.motion_features.sequence();
            if (seq != seen_motion_seq) {
                // Comparing by difference stays correct across a 2^32 wrap
                // (latest_value.h); a jump of 2+ means windows were
                // overwritten unread — stepping with the newest one makes
                // the detector see the t_ms gap and hold its state until
                // evidence re-confirms (F04 D1: no evidence, no change).
                seen_motion_seq = seq;

                bw::MotionFeatures motion{};
                bw::PpgFeatures ppg{};
                bw::wear::WearInput input{};
                input.t_ms = ctx.clock->nowMs();
                if (ctx.motion_features.get(motion)) {
                    input.motion = &motion;
                }
                // PPG only when its window is co-tick with the motion one:
                // F02 publishes both at window close, so equal sequences
                // mean "same window". A stalled or absent PPG (the bench's
                // PpgAbsent mode) leaves the pointer null — an absent
                // source, never a stale reading re-labelled as fresh.
                if (seq == ctx.ppg_features.sequence() && ctx.ppg_features.get(ppg)) {
                    input.ppg = &ppg;
                }
                ctx.wear_estimate.set(g_detector.step(input));
            }
            // No new window: do NOT re-step — identical evidence must not
            // advance the confirm dwell a second time.
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Wear, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kWearPumpPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction happens at file scope; the call exists so the
    // integrator's fixed wire() order stays uniform. Task table landed at
    // P4 (2026-10-12): wire() also creates the 1 Hz pump that reads
    // AppContext's feature mailboxes and publishes wear_estimate. The pump
    // may run before F02 publishes anything: sequence() stays 0, the loop
    // just waits — an empty channel is an ordinary state, never a stall.
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(pumpTask, "F04Pump", kWearPumpStackBytes, &ctx,
                                                     kWearPumpPriority, nullptr, kWearPumpCore),
                             bw::diag::TaskSlot::Wear, ctx.faults);
#else
    (void)ctx;
#endif
    (void)detector();
}

bw::wear::WearDetector& detector() {
    return g_detector;
}

}  // namespace f04
