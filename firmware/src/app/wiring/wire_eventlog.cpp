#include "wire_eventlog.h"
#include "task_health.h"

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the event pump (device build only)
#endif

namespace f17 {

namespace {

// The logger is constructed on the FIRST wire() call — before the pump task
// exists — so ctx.clock/ctx.faults reach it by dependency injection instead of
// static-init order; no placement new, no heap. Later calls return the same
// instance and ignore the arguments (F08 trackerImpl pattern), which is what
// makes wire() idempotent and its Boot emission exactly-once (§15.2 D10).
bw::eventlog::EventLogger& loggerImpl(const bw::IClock* clock, bw::IFaultReporter* faults) {
    static bw::eventlog::EventLogger s_logger{clock, faults};
    return s_logger;
}

// Composition-root view of the same instance (sink()/stats()/pumpOnce()).
bw::eventlog::EventLogger& logger() {
    return loggerImpl(nullptr, nullptr);
}

// Exactly one pump task may ever exist. wire() is documented idempotent, but
// without this a second call would spawn a second drain task racing the first
// on the same queue — which would make the "idempotent" claim in wire()'s
// contract false (audit L-4). The claim itself is evaluated on EVERY wire()
// (device and host alike) so the guard is host-testable; only the
// xTaskCreatePinnedToCore that consumes it is behind ARDUINO.
uint32_t s_pump_claims = 0;

bool claimPump() {
    if (s_pump_claims != 0) {
        return false;
    }
    ++s_pump_claims;
    return true;
}

}  // namespace

void wire(app::AppContext& ctx) {
    // Build first (clock/faults injected), THEN publish Boot: the Boot record
    // is stamped through the real clock when one exists (§15.2 D10).
    loggerImpl(ctx.clock, ctx.faults).emitBoot();
    if (claimPump()) {
#if defined(ARDUINO)
        app::tasks::checkCreated(
            xTaskCreatePinnedToCore(eventLogTask, "F17Log", kEventPumpStackBytes, &ctx,
                                    kEventPumpPriority, nullptr, kEventPumpCore),
            bw::diag::TaskSlot::EventLog, ctx.faults);
#endif
    }
}

uint32_t pumpClaims() {
    return s_pump_claims;
}

void pumpOnce(bw::TimeMs now_ms) {
    (void)logger().drain(now_ms);
}

bw::IEventSink* sink() {
    return &logger();
}

void attachStore(bw::IEventStore* store) {
    logger().attachStore(store);
}

void attachStore(bw::IEventStore* store, uint32_t last_seq_hint) {
    logger().attachStore(store, last_seq_hint);
}

bool attachSubscriber(bw::IEventSink* subscriber) {
    return logger().addSubscriber(subscriber);
}

bw::eventlog::EventLogStats stats() {
    return logger().stats();
}

#if defined(ARDUINO)

void eventLogTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil) — a late tick shifts only itself; max lateness stays a
    // diagnostic, not a failure. The clock is read here so the backoff horizon
    // (§10) uses fresh monotonic time every cycle.
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        const bw::TimeMs now = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
        pumpOnce(now);
        app::tasks::completeCycle(bw::diag::TaskSlot::EventLog, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kEventPumpPeriodMs));
    }
}

#endif  // ARDUINO

}  // namespace f17
