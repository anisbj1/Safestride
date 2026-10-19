#include "wire_state.h"
#include "task_health.h"

#include <bw/state/state_pump.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for StateTask (device build only)
#endif

namespace f07 {

namespace {

// Static-storage graph (no heap: constructed before any task runs). The pump
// holds the engine plus all pull/publish logic as pure logic (bw::state::
// StatePump, spec D15) so the host suite can drive it with a FakeClock; this
// file is the platform shell only (01 §1 rule 3).
bw::state::StatePump s_pump{};
bw::state::StateProvider s_provider{};

// Pipeline health (H7), written by the task only.
uint32_t s_max_lateness_ms = 0;

}  // namespace

#if defined(ARDUINO)

void stateTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil): a late tick shifts only itself, so the decision grid
    // stays on the 1 s window publish phase — same as F02/F03's pumps.
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        if (late_ms > s_max_lateness_ms) {
            s_max_lateness_ms = late_ms;
        }

        // No clock yet: honest wait (F02's rule) — never step with t=0 and
        // never fabricate a timeline. The pump does everything else
        // (sequence-gated pulls, quality aging, engine step, publish).
        if (ctx.clock != nullptr) {
            (void)s_pump.tick(ctx.clock->nowMs());
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::State, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kStateTaskPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction + ctx-side port binding. The StateEngine/StatePump exist
    // at file scope with the F07 §8 defaults. The three W1 mailboxes are
    // bound by attachMailboxes at P4 (wire_state.h); the task starts even
    // before that: never-received inputs gate to Unknown + SensorUnavailable
    // — a tested state (F07 §9) — and every publish is guarded by its port.
    bw::state::PumpPorts& p = s_pump.ports();
    p.motion = &ctx.motion_features;
    p.ppg = &ctx.ppg_features;
    p.activity = &ctx.activity_estimate;
    p.wake_check_passed = &ctx.wake_check_passed_ms;
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(stateTask, "F07State", kStateTaskStackBytes,
                                                     &ctx, kStateTaskPriority, nullptr,
                                                     kStateTaskCore),
                             bw::diag::TaskSlot::State, ctx.faults);
#else
    (void)ctx;
#endif
}

void attachMailboxes(const bw::LatestValue<bw::WearEstimate>* wear,
                     const bw::LatestValue<bw::AlertnessProbabilities>* alertness,
                     bw::LatestValue<bw::StateSnapshot>* snapshot) {
    bw::state::PumpPorts& p = s_pump.ports();
    p.wear = wear;
    p.alertness = alertness;
    p.snapshot = snapshot;
    s_provider.bind(snapshot);
}

void attachEventSink(bw::IEventSink* sink) {
    s_pump.ports().events = sink;
}

void attachQualityProvider(bool (*provider)(bw::SourceQuality& out)) {
    // Single-writer composition root (same rule as attachMailboxes): bound
    // once beside f07::wire(ctx); nullptr restores the A-03 derivation.
    s_pump.ports().quality_provider = provider;
}

void attachModeProvider(const bw::IModeProvider* provider) {
    s_pump.ports().mode_provider = provider;
}

bw::state::StateEngine& engine() {
    return s_pump.engine();
}

bw::state::StateProvider& provider() {
    return s_provider;
}

bw::TimeMs lastRunMs() {
    return s_pump.lastRunMs();
}

uint32_t maxLatenessMs() {
    return s_max_lateness_ms;
}

uint32_t droppedEvents() {
    return s_pump.droppedEvents();
}

}  // namespace f07
