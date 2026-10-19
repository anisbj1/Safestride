#include "wire_cataplexy.h"
#include "task_health.h"

#include <bw/cataplexy/candidate_event.h>  // makeSuspectedCataplexyEvent (pure builder)

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the pump (device build only)
#endif

namespace f09 {

namespace {

// Static-storage instances (no heap: construction happens before any task
// runs, embedded conventions). Threshold defaults live in bw::cataplexy's
// named constants (F09 §8).
bw::cataplexy::EventCandidateDetector s_detector{};
bw::cataplexy::EventWindowRequester s_window_requester{};

// Event sink (F17 slot) and pump-only drop counter — written by pumpTask,
// read by droppedEvents() (f07::droppedEvents precedent, D11: never block
// the decision path on a logger that does not exist yet).
bw::IEventSink* s_events = nullptr;
uint32_t s_dropped_events = 0;

}  // namespace

#if defined(ARDUINO)

void pumpTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil) — the pump grid stays phase-locked to F02's 1 Hz
    // window publishes, which is what keeps the DetectorInput set co-tick.
    TickType_t last_wake = xTaskGetTickCount();

    // Motion sequence of the last window stepped (0 = nothing yet).
    uint32_t seen_motion_seq = 0;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // No clock yet: honest wait (F02/F07 rule) — the drop rule judges
        // over window gaps, never a fabricated t = 0 timeline.
        if (ctx.clock != nullptr) {
            const uint32_t seq = ctx.motion_features.sequence();
            if (seq != seen_motion_seq) {
                seen_motion_seq = seq;

                bw::MotionFeatures motion{};
                if (ctx.motion_features.get(motion)) {
                    bw::cataplexy::DetectorInput in{};
                    in.t_ms = ctx.clock->nowMs();
                    in.motion = &motion;

                    bw::PpgFeatures ppg{};
                    if (seq == ctx.ppg_features.sequence() && ctx.ppg_features.get(ppg)) {
                        in.ppg = &ppg;  // co-tick window; absent PPG stays null
                    }
                    bw::StateSnapshot snapshot{};
                    if (ctx.state_snapshot.get(snapshot)) {
                        in.state = &snapshot;  // freshest 1 Hz snapshot
                    }

                    const bw::cataplexy::CandidateOutput out = s_detector.step(in);
                    if (out.raised) {
                        const bw::Event event = bw::cataplexy::makeSuspectedCataplexyEvent(out);
                        // Null sink (F17 not landed) or a rejected publish:
                        // drop and count, never block the pump (D11).
                        if (s_events == nullptr || s_events->publish(event).err != bw::Err::Ok) {
                            ++s_dropped_events;
                            app::tasks::reportEventDrop(ctx.faults, in.t_ms);
                        }
                    }
                }
            }
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Cataplexy, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kCataplexyPumpPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction only — both objects are already built with the F09 §8
    // defaults. Task table landed at P4 (2026-10-12): create the 1 Hz pump
    // that steps the detector from AppContext's channels. Before F02/F07
    // publish anything the pump just waits — an empty channel is an
    // ordinary state, never a fabricated candidate.
    //
    // Fault reporting for the disabled detector (F09 §10, SensorUnavailable)
    // still waits: the detector reports the condition as
    // CandidateOutput::disabled for whoever pumps it.
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(pumpTask, "F09Pump", kCataplexyPumpStackBytes,
                                                     &ctx, kCataplexyPumpPriority, nullptr,
                                                     kCataplexyPumpCore),
                             bw::diag::TaskSlot::Cataplexy, ctx.faults);
#else
    (void)ctx;
#endif
}

void attachEventSink(bw::IEventSink* sink) {
    s_events = sink;
}

uint32_t droppedEvents() {
    return s_dropped_events;
}

bw::cataplexy::EventCandidateDetector& detector() {
    return s_detector;
}

bw::cataplexy::EventWindowRequester& windowRequester() {
    return s_window_requester;
}

}  // namespace f09
