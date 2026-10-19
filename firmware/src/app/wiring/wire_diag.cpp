#include "wire_diag.h"
#include "task_health.h"

#include <atomic>

#include <bw/containers/spsc_queue.h>
#include <bw/diag/diag_types.h>
#include <bw/diag/diagnostics.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API + ESP heap stats (device build only)

#include "wire_ble.h"  // f19::linkProvider() - CR-018 link state (D12)
#include "wire_hal.h"  // f01::imu/ppg/battery - composition-level binding (README §1)
#endif

namespace f23 {

namespace {

// Static-storage graph (no heap, no init-order dependency): the composer
// with the F23 §8 defaults, plus the D5 output mailboxes. All behavior
// lives in bw_diag; this file is the platform shell only (01 §1 rule 3).
bw::diag::Diagnostics s_diag{};
bw::LatestValue<bw::SourceQuality> s_quality{};
bw::LatestValue<bw::HealthSnapshot> s_health{};
struct SensorStatuses {
    bw::SensorStatus imu;
    bw::SensorStatus ppg;
};
bw::LatestValue<SensorStatuses> s_sensor_statuses;

// --- D13 per-task registry ---------------------------------------------------
// One mailbox slot per bw::diag::TaskSlot (single writer per slot) plus
// the tick snapshot array gatherInput() refreshes each tick — DiagInput
// carries pointer+count into it (no per-tick struct copy beyond this).
bw::LatestValue<bw::TaskHealth> s_task_slots[bw::diag::kMaxTrackedTasks]{};
bw::TaskHealth s_task_snapshot[bw::diag::kMaxTrackedTasks]{};

// --- D15 raw tee -------------------------------------------------------------
// F23-owned SPSC queues (producer: f23::onImuSample/onPpgSample from
// F01's SensorTask, P4; consumer: gatherInput below) + static drain
// windows — the task stack would blow its budget with them (§16.1).
bw::SpscQueue<bw::ImuSample, bw::diag::kDiagRawQueueImuSamples> s_imu_tee{};
bw::SpscQueue<bw::PpgSample, bw::diag::kDiagRawQueuePpgSamples> s_ppg_tee{};
bw::ImuSample s_imu_window[bw::diag::kDiagRawQueueImuSamples]{};
bw::PpgSample s_ppg_window[bw::diag::kDiagRawQueuePpgSamples]{};

// D15 drop-hole state: the drop counts observed at the previous drain. A
// change since then = a hole in the stream ⇒ that source's window is
// suppressed this tick (§15.4 absent-window rule ends the raw episode).
uint32_t s_imu_drops_seen = 0;
uint32_t s_ppg_drops_seen = 0;

// D10: F26's CR-030 seam — nullptr (unwired) is honest; value is copied
// into DiagInput once per tick, never handed to pure logic as a pointer.
std::atomic<bw::IReliabilityStateProvider*> s_reliability{nullptr};

// Bracket the complete drain, not just its start: the producer can refuse a
// sample while the consumer is draining. Such a hole must suppress this window.
template <class Queue, class Sample>
size_t drainRawWindow(Queue& queue, Sample* window, size_t capacity, uint32_t& drops_seen) {
    const uint32_t before = queue.drops();
    size_t count = 0;
    while (count < capacity && queue.pop(window[count])) {
        ++count;
    }
    const uint32_t after = queue.drops();
    const bool hole = before != drops_seen || after != before;
    drops_seen = after;
    return hole ? 0 : count;
}

// D4/§15.4: gather the wiring-provided system stats. valid stays false on
// native (no ESP heap) — honest unknowns, never a fabricated zero-fault
// diagnosis. W6 P4 task lateness uses the per-task registry exclusively;
// SystemHealthMonitor merges its valid slots without racing legacy counters.
bw::diag::SystemStats gatherStats() {
    bw::diag::SystemStats stats{};
#if defined(ARDUINO)
    stats.valid = true;
    stats.min_free_heap = ESP.getMinFreeHeap();  // running min == §15.4 field
    // W6 P4: all task lateness now arrives through the tear-free registry.
    // Never race F07's legacy plain counter from another task.
    // D12: F19's last link-state snapshot, reported AS-IS — no staleness
    // gate (CR-018 snapshots are event-time; a quiet-but-connected phone
    // legitimately has an old t_ms, so gating would fabricate
    // disconnects). Null provider (bench, pre-wire) ⇒ false, honest.
    if (bw::IBleLinkProvider* link = f19::linkProvider(); link != nullptr) {
        bw::BleLinkState state{};
        stats.ble_connected = link->latest(state) && state.connected;
    }
#endif
    return stats;
}

}  // namespace

void noteTaskCycle(bw::diag::TaskSlot slot, bw::TimeMs now_ms, uint32_t lateness_ms,
                   uint16_t stack_free_bytes) {
    const uint8_t index = static_cast<uint8_t>(slot);
    if (index >= bw::diag::kMaxTrackedTasks) {
        return;  // TaskSlot::Count / bogus value: defined no-op
    }
    // Single-writer-per-slot RMW (D13): get → noteCycle (max-keeps
    // lateness) → set. get() leaves the value untouched before the first
    // note (default valid=false), so a fresh slot starts at "no data".
    // LatestValue's critical section keeps the copy tear-free; the writer
    // is this slot's task alone.
    bw::TaskHealth health{};
    (void)s_task_slots[index].get(health);
    health.noteCycle(now_ms, lateness_ms, stack_free_bytes);
    s_task_slots[index].set(health);
}

bool latestTaskHealth(bw::diag::TaskSlot slot, bw::TaskHealth& out) {
    const uint8_t index = static_cast<uint8_t>(slot);
    if (index >= bw::diag::kMaxTrackedTasks) {
        return false;
    }
    return s_task_slots[index].get(out);
}

void attachReliabilityProvider(bw::IReliabilityStateProvider* provider) {
    s_reliability.store(provider, std::memory_order_release);
}

void noteSensorStatuses(bw::SensorStatus imu, bw::SensorStatus ppg) {
    s_sensor_statuses.set({imu, ppg});
}

bool latestSensorStatuses(bw::SensorStatus& imu, bw::SensorStatus& ppg) {
    SensorStatuses statuses{};
    if (!s_sensor_statuses.get(statuses)) {
        return false;
    }
    imu = statuses.imu;
    ppg = statuses.ppg;
    return true;
}

void onImuSample(const bw::ImuSample& sample) {
    // Err::Full is refused + counted by SpscQueue::drops() (CR-004); the
    // drop count is what the D15 drop-hole rule reads — never ignored.
    (void)s_imu_tee.push(sample);
}

void onPpgSample(const bw::PpgSample& sample) {
    (void)s_ppg_tee.push(sample);
}

bw::diag::DiagInput gatherInput(const app::AppContext& ctx, bw::TimeMs now_ms) {
    bw::diag::DiagInput in{};
    in.now_ms = now_ms;
    SensorStatuses sensors{};
    if (s_sensor_statuses.get(sensors)) {
        in.has_imu_status = true;
        in.imu_status = sensors.imu;
        in.has_ppg_status = true;
        in.ppg_status = sensors.ppg;
    }
#if defined(ARDUINO)
    in.battery = f01::battery().read();  // valid=false on bench (A-04) — a flag
#endif
    bw::MotionFeatures motion{};
    if (ctx.motion_features.get(motion)) {
        in.has_motion = true;
        in.motion = motion;
    }
    bw::PpgFeatures ppg{};
    if (ctx.ppg_features.get(ppg)) {
        in.has_ppg = true;
        in.ppg = ppg;
    }
    in.stats = gatherStats();

    // D10: one value copy per tick (D4 precedent). No snapshot yet ⇒
    // has_reliability stays false ⇒ D11 own-only fallback (honest).
    if (auto* provider = s_reliability.load(std::memory_order_acquire); provider != nullptr) {
        bw::ReliabilityState reliability{};
        if (provider->latest(reliability)) {
            in.has_reliability = true;
            in.reliability = reliability;
        }
    }

    // D13: refresh the tick snapshot from the mailboxes (get() leaves the
    // destination untouched before the first note — the slot then stays
    // default valid=false = excluded, unknown ≠ 0).
    for (uint8_t i = 0; i < bw::diag::kMaxTrackedTasks; ++i) {
        (void)s_task_slots[i].get(s_task_snapshot[i]);
    }
    in.tasks = s_task_snapshot;
    in.task_count = bw::diag::kMaxTrackedTasks;

    // D15 drain: pop up to the window capacity into the static arrays
    // (always — even when suppressed — so a hole never clogs the queue).
    // Drop-hole rule: drops() changed since the last tick ⇒ a sample was
    // refused ⇒ this source's window is suppressed (nullptr/0) and the
    // §15.4 absent-window rule ends the raw episode — a hole is never
    // bridged into a fictitious identical-run.
    const size_t imu_n = drainRawWindow(s_imu_tee, s_imu_window, bw::diag::kDiagRawQueueImuSamples,
                                        s_imu_drops_seen);
    if (imu_n > 0) {
        in.imu_raw = s_imu_window;
        in.imu_raw_n = imu_n;
    }

    const size_t ppg_n = drainRawWindow(s_ppg_tee, s_ppg_window, bw::diag::kDiagRawQueuePpgSamples,
                                        s_ppg_drops_seen);
    if (ppg_n > 0) {
        in.ppg_raw = s_ppg_window;
        in.ppg_raw_n = ppg_n;
    }

    return in;
}

namespace {

// One 1 Hz tick: step the composer, publish the D5 mailboxes, forward the
// D9 report edges. A missing fault sink drops the edge (F01 D7 fallback):
// diagnostics never blocks on F26 being wired.
void runTick(app::AppContext& ctx, const bw::diag::DiagInput& in) {
    const bw::diag::DiagOutput out = s_diag.step(in);
    s_quality.set(out.quality);
    s_health.set(out.health);
    // P4 (2026-10-12, integrator): app.h grew the `health` field, so the
    // snapshot also goes to the ctx mailbox wire_ui.cpp binds via its SFINAE
    // seam (F12's diagnostics screen). Same value, one publisher, no logic.
    ctx.health.set(out.health);
    for (uint8_t i = 0; i < out.edge_count; ++i) {
        if (ctx.faults != nullptr) {
            ctx.faults->report(out.edges[i].code, out.edges[i].severity, in.now_ms);
        }
    }
}

}  // namespace

#if defined(ARDUINO)

void diagTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil): a late tick shifts only itself; its lateness is
    // measured because TaskOverrun diagnoses exactly that (§15.3).
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const TickType_t wake = xTaskGetTickCount();
        const uint32_t late_ms = static_cast<uint32_t>(wake - last_wake) * portTICK_PERIOD_MS;

        // No clock yet: honest wait (F02/F07 rule) — never step with t=0
        // and never fabricate a timeline.
        if (ctx.clock != nullptr) {
            const bw::TimeMs now_ms = ctx.clock->nowMs();
            // D13/D14: DiagTask reports into its OWN registry slot
            // (kDiagSelfTaskSlot), replacing the M2 file-local
            // self-lateness counter — noteCycle keeps the worst lateness.
            // Watermark: IDF's uxTaskGetStackHighWaterMark returns BYTES
            // on this platform (kStackWatermarkUnitBytes, header-verified).
            runTick(ctx, gatherInput(ctx, now_ms));
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::DiagSelf, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kDiagTaskPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction (the static graph exists at file scope) + task
    // creation (README §1: no behavior here).
#if defined(ARDUINO)
    if (xTaskCreatePinnedToCore(diagTask, "F23Diag", kDiagTaskStackBytes, &ctx, kDiagTaskPriority,
                                nullptr, kDiagTaskCore) != pdPASS) {
        reportTaskCreateFailure(ctx, ctx.clock != nullptr ? ctx.clock->nowMs() : 0);
    }
#else
    (void)ctx;
#endif
}

void reportTaskCreateFailure(const app::AppContext& ctx, bw::TimeMs now_ms) {
    if (ctx.faults != nullptr) {
        // [DERIVED] Diagnostics is unavailable, but acquisition/alerts remain
        // alive: Error (reduced capability), not a fabricated heartbeat stall.
        constexpr bw::FaultSeverity kDiagCreateFailureSeverity = bw::FaultSeverity::Error;
        ctx.faults->report(bw::FaultCode::TaskCreateFailed, kDiagCreateFailureSeverity, now_ms);
    }
}

bool latestSourceQuality(bw::SourceQuality& out) {
    return s_quality.get(out);
}

bool latestHealth(bw::HealthSnapshot& out) {
    return s_health.get(out);
}

}  // namespace f23
