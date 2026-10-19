#include "wire_sleep.h"
#include "task_health.h"

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the episode pump (device build only)
#endif

namespace f08 {

namespace {

// IStateProvider over the app-owned state_snapshot mailbox (bw_core only, 01
// §1 rule 1): F08 consumes F07's output as a contract, never as a concrete
// class. latest() returns false until F07 has published its first snapshot —
// the pump then simply waits (stale stream = nothing advances, F08 §10).
class SnapshotProvider final : public bw::IStateProvider {
public:
    void bind(const bw::LatestValue<bw::StateSnapshot>* mailbox) { mailbox_ = mailbox; }

    bool latest(bw::StateSnapshot& out) const override {
        return mailbox_ != nullptr && mailbox_->get(out);
    }

private:
    const bw::LatestValue<bw::StateSnapshot>* mailbox_ = nullptr;
};

// Static-initialization-order note (audit I-4): everything below at namespace
// scope is CONSTANT-initialized (zero or trivial default initialization — no
// dynamic initializer exists for any of it, including the pending-event ring),
// so all of it is valid before any dynamic initialization runs in any
// translation unit and no init order relative to AppContext or other wiring
// statics matters. The tracker is deliberately different: its construction is
// DYNAMIC, so it is deferred to the first wire() call at runtime (trackerImpl
// below) instead of happening during static init.
SnapshotProvider s_provider{};
bw::IEventSink* s_sink = nullptr;
bw::LatestValue<bw::EpisodeStatus>* s_status = nullptr;

// --- M-1 order-independent event delivery --------------------------------------
// Audit M-1: wire() runs recover() (and starts the pump) possibly BEFORE
// attachEventSink() has run — the P4 attach hook is called at the
// composition root's discretion. A recovered-episode event delivered into a
// null sink was silently dropped. Events produced while detached are
// buffered here and flushed on attach, so delivery no longer depends on
// attach order (§7, §10).
//
// [DERIVED] Why 8: at most ONE recovery event exists per boot; between
// wire() and a late attach the pump can close at most a handful of episodes
// (each needs kMinEpisodeMs of evidence), so 8 bounds a pathological burst
// without a container. Overflow drops the OLDEST event — the state machine
// is never blocked and a dropped publish stays counted by F17's counters
// (§10), exactly like a rejected publish.
constexpr size_t kPendingEventCapacity = 8;
bw::Event s_pending_events[kPendingEventCapacity] = {};
size_t s_pending_head = 0;  // ring index of the oldest buffered event
size_t s_pending_count = 0;

// The tracker is constructed on the FIRST call — wire(), before the pump task
// exists — so ctx.clock reaches it by dependency injection instead of static-
// init order; no placement new, no heap. Later calls return the same instance
// and ignore the argument.
bw::sleep::SleepEpisodeTracker& trackerImpl(const bw::IClock* wall_clock) {
    static bw::sleep::SleepEpisodeTracker s_tracker{bw::sleep::SleepEpisodeConfig{}, nullptr,
                                                    wall_clock};
    return s_tracker;
}

// One delivery path for step() and recover() results: publish events, and
// touch the status mailbox ONLY on status_changed (F08 §7 — writing every tick
// would inflate LatestValue::overwrites(), a real data-loss counter).
// Events produced while no sink is attached are BUFFERED, not dropped (M-1):
// attachEventSink() below flushes them, so delivery is independent of the
// attach order.
void deliver(const bw::sleep::StepOutput& out) {
    if (out.status_changed && s_status != nullptr) {
        s_status->set(out.status);
    }
    if (!out.has_event) {
        return;
    }
    if (s_sink != nullptr) {
        // §10: a rejected publish is not retried into a busy log — the drop is
        // already counted by F00's/F17's counters.
        const bw::Result<void> published = s_sink->publish(out.event);
        (void)published;
        return;
    }
    if (s_pending_count == kPendingEventCapacity) {
        // Full: drop the OLDEST buffered event (ring) — the state machine is
        // never blocked by a missing consumer (§10), and a burst this large
        // cannot occur before a P4 attach (see kPendingEventCapacity).
        s_pending_head = (s_pending_head + 1) % kPendingEventCapacity;
        --s_pending_count;
    }
    s_pending_events[(s_pending_head + s_pending_count) % kPendingEventCapacity] = out.event;
    ++s_pending_count;
}

}  // namespace

bw::sleep::SleepEpisodeTracker& tracker() {
    return trackerImpl(nullptr);
}

void wire(app::AppContext& ctx) {
    s_provider.bind(&ctx.state_snapshot);
    // Wall clock (§6 D2): ctx.clock is the device IClock — F00 §13 open, so it
    // is nullptr until assigned; the tracker degrades to "wall time unknown"
    // while that holds (DEC-009). kv stays nullptr too: the wave-2 build has
    // no F18 store yet (§6 D4/§13), and recovery degrades to "no marker".
    trackerImpl(ctx.clock);
    // §6 D4: exactly once, at init, before the first step() ever runs. If no
    // sink is attached yet, deliver() buffers the result (M-1) and
    // attachEventSink() flushes it — order-independent delivery.
    deliver(tracker().recover());
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(sleepTask, "F08Sleep", kSleepPumpStackBytes,
                                                     &ctx, kSleepPumpPriority, nullptr,
                                                     kSleepPumpCore),
                             bw::diag::TaskSlot::Sleep, ctx.faults);
#endif
}

#if defined(ARDUINO)

void sleepTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);
    (void)ctx;

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil) — a late tick shifts only itself; the episode grid is
    // second-granularity, max lateness is a diagnostic, not a failure.
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        pumpOnce();
        app::tasks::completeCycle(bw::diag::TaskSlot::Sleep, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSleepPollPeriodMs));
    }
}

#endif  // ARDUINO

// One poll → step → deliver cycle. The pump task calls it every
// kSleepPollPeriodMs; host tests (README §3: the wiring pair builds natively)
// call it directly to drive the full wiring path — provider → tracker →
// buffered delivery — without FreeRTOS.
void pumpOnce() {
    bw::StateSnapshot snapshot{};
    if (s_provider.latest(snapshot)) {
        deliver(tracker().step(snapshot));
    }
}

void attachEventSink(bw::IEventSink* sink) {
    s_sink = sink;
    if (s_sink == nullptr) {
        return;  // still detached: the pending events stay buffered (M-1)
    }
    // M-1 flush: events produced before this attach (wire()'s recover(), or
    // early pump closes) go out now, in order. A rejection here follows §10
    // too: dropped, never retried — F17's counters own the drop.
    while (s_pending_count > 0) {
        const bw::Event& oldest = s_pending_events[s_pending_head];
        const bw::Result<void> published = s_sink->publish(oldest);
        (void)published;
        s_pending_head = (s_pending_head + 1) % kPendingEventCapacity;
        --s_pending_count;
    }
}

void attachEpisodeStatus(bw::LatestValue<bw::EpisodeStatus>* status) {
    s_status = status;
}

}  // namespace f08
