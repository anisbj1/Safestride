// firmware/src/app/wiring/wire_baseline.cpp — F13's construction only
// (construction, injection, task body; no learning logic — that is bw_baseline).

#include "wire_baseline.h"
#include "task_health.h"

#include <bw/baseline/baseline_serializer.h>

#include "../app.h"  // AppContext mailboxes read by the pump

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the pump (device build only)

#include "wire_storage.h"  // f18::kvStore() — composition-level binding (README §1)
#endif

namespace f13 {

namespace {

bw::baseline::BaselineTracker* s_tracker = nullptr;
BaselineProviderAdapter* s_adapter = nullptr;
bw::IKeyValueStore* s_store = nullptr;  // persist target; nullptr keeps learning in memory (§10)

// Pump-owned runtime cursors; never persisted across a reboot.
bw::TimeMs s_seen_ppg_ms = 0;
bw::TimeMs s_seen_motion_ms = 0;
bool s_consumed_pair = false;
bw::TimeMs s_last_persist_ms = 0;
bw::BaselineProfile s_last_published{};
bool s_published_any = false;

// D6 / §10: an unusable stored blob is discarded WITH a fault (CR-031 landed at
// W6 G1: FaultCode::BaselineCorrupt = 25, severity Warning per the CR text).
// Null fault sink or null clock degrade the report, never the boot (F01 D7
// fallback pattern — no feature may assume a sink exists).
void reportCorruptBaseline(const app::AppContext& ctx) {
    if (ctx.faults == nullptr) {
        return;
    }
    const bw::TimeMs t = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
    ctx.faults->report(bw::FaultCode::BaselineCorrupt, bw::FaultSeverity::Warning, t);
}

}  // namespace

void wireWithStore(app::AppContext& ctx, bw::IKeyValueStore* store) {
    // Function-local statics: constructed once, no heap after startup (00 §7.2).
    // Re-assignment gives wire() its idempotence contract — every call is
    // "fresh construction + one load attempt", so a second call can never stack
    // state on top of the first (README call-once rule, testable per call).
    static bw::baseline::BaselineTracker instance;
    static BaselineProviderAdapter adapter;
    instance = bw::baseline::BaselineTracker{};
    adapter.reset();
    s_tracker = &instance;
    s_adapter = &adapter;
    s_store = store;
    s_seen_ppg_ms = 0;
    s_seen_motion_ms = 0;
    s_consumed_pair = false;
    s_last_persist_ms = 0;
    s_last_published = {};
    s_published_any = false;

    if (store == nullptr) {
        return;  // no store yet ⇒ in-memory-only boot, honest (§10)
    }

    bw::baseline::TrackerState restored{};
    const bw::Result<void> loaded =
        bw::baseline::BaselineSerializer::loadFromStore(store, restored);
    if (loaded.ok()) {
        const bw::Result<void> accepted = s_tracker->restore(restored);
        if (!accepted.ok()) {
            reportCorruptBaseline(ctx);  // well-formed CRC but absurd values ⇒ same path
        } else {
            bw::BaselineProfile p{};
            if (s_tracker->profile(p)) {
                adapter.publish(p);  // restored baseline readable before the first tick
            }
        }
        return;
    }
    // NotFound (first boot) and transient store errors are ordinary states
    // (§10: keep learning in memory, persist later). Only an unreadable or
    // version-incompatible blob reports the CR-031 fault; the tracker is
    // already fresh, which IS the "discard and restart learning" behavior.
    if (loaded.err == bw::Err::Corrupt || loaded.err == bw::Err::Unsupported ||
        loaded.err == bw::Err::Full) {
        reportCorruptBaseline(ctx);
    }
}

void wire(app::AppContext& ctx) {
#if defined(ARDUINO)
    // Composition-level binding (README §1): the storage slot ran first, so
    // f18::kvStore() is mounted; a present-but-unreadable medium still lands
    // in the honest in-memory path inside wireWithStore (§10).
    wireWithStore(ctx, &f18::kvStore());
#else
    // Host: no composition root to resolve f18 from — the tested no-store boot.
    wireWithStore(ctx, nullptr);
#endif
}

bw::IBaselineProvider& provider() {
    return *s_adapter;
}

bw::baseline::BaselineTracker& tracker() {
    return *s_tracker;
}

namespace {

// Field-wise comparison, not memcmp: BaselineProfile carries padding and the
// padding is not part of the answer (§9 test 16 pins the 20 B layout).
bool sameProfile(const bw::BaselineProfile& a, const bw::BaselineProfile& b) {
    return a.valid == b.valid && a.resting_hr_bpm == b.resting_hr_bpm &&
           a.resting_hr_mad == b.resting_hr_mad && a.typical_enmo_g == b.typical_enmo_g &&
           a.confidence_pct == b.confidence_pct && a.version == b.version;
}

}  // namespace

void pumpOnce(app::AppContext& ctx) {
    // Observe holds even if sensor evidence is absent/stale. Otherwise an
    // event during a sensor gap would never arm the post-event cooldown.
    if (s_tracker == nullptr || s_adapter == nullptr || ctx.clock == nullptr) {
        return;
    }
    bw::AlertStatus alert{};
    bw::EpisodeStatus episode{};
    if ((ctx.alert_status.get(alert) && alert.level >= bw::AlertLevel::Active) ||
        (ctx.episode_status.get(episode) && episode.phase != bw::EpisodePhase::Idle)) {
        bw::baseline::BaselineSample held{};
        held.hold = true;
        (void)s_tracker->step(held);  // HoldActive arms cooldown; no evidence learned.
        return;
    }
    bw::MotionFeatures motion{};
    bw::PpgFeatures ppg{};
    bw::ActivityEstimate activity{};
    bw::WearEstimate wear{};

    // F13 §7: skip the tick entirely if any of the four is absent —
    // a zeroed window would read as Good + 0 g = fabricated rest.
    if (ctx.motion_features.get(motion) && ctx.ppg_features.get(ppg) &&
        ctx.activity_estimate.get(activity) && ctx.wear_estimate.get(wear)) {
        const bw::TimeMs now = ctx.clock->nowMs();

        // Freshness is the pump's job (§7, F06 precedent): windows older
        // than kMaxSampleAgeMs describe the past, not this user's rest.
        const bool ppg_fresh =
            (now >= ppg.t_ms) && (now - ppg.t_ms) <= bw::baseline::kMaxSampleAgeMs;
        const bool motion_fresh =
            (now >= motion.t_ms) && (now - motion.t_ms) <= bw::baseline::kMaxSampleAgeMs;

        const bool context_fresh = now >= activity.t_ms && now >= wear.t_ms &&
                                   now - activity.t_ms <= bw::baseline::kMaxSampleAgeMs &&
                                   now - wear.t_ms <= bw::baseline::kMaxSampleAgeMs;
        // Both windows must advance; polling twice or republishing a
        // held sensor value is not two seconds of qualified evidence.
        const bool new_pair =
            !s_consumed_pair || (ppg.t_ms > s_seen_ppg_ms && motion.t_ms > s_seen_motion_ms);

        if (ppg_fresh && motion_fresh && context_fresh && new_pair) {
            s_consumed_pair = true;
            s_seen_ppg_ms = ppg.t_ms;
            s_seen_motion_ms = motion.t_ms;
            bw::baseline::BaselineSample sample{};
            sample.hr_bpm = ppg.hr_bpm;
            sample.hr_valid = ppg.hr_valid;
            sample.motion_corrupted = ppg.motion_corrupted;
            sample.ppg_quality = ppg.quality;
            sample.enmo_g = motion.enmo_g;
            sample.motion_quality = motion.quality;
            sample.wear = wear.state;
            sample.activity = activity.context;

            bw::StateSnapshot snapshot{};
            const bool has_snapshot = ctx.state_snapshot.get(snapshot);
            if (has_snapshot &&
                (now < snapshot.t_ms || now - snapshot.t_ms > bw::baseline::kMaxSampleAgeMs)) {
                return;  // Old quality/mode cannot qualify current evidence.
            }
            // Absent snapshot ⇒ Good: missing context alone must not
            // veto a sample (§7), and absent mode ⇒ calibration=false
            // ⇒ the normal gate (D2).
            sample.snapshot_ppg = has_snapshot ? snapshot.quality.ppg : bw::DataQuality::Good;
            sample.snapshot_imu = has_snapshot ? snapshot.quality.imu : bw::DataQuality::Good;
            sample.calibration = has_snapshot && snapshot.mode == bw::OperatingMode::Calibration;

            const bw::baseline::StepResult result = s_tracker->step(sample);

            if (result.accepted) {
                bw::BaselineProfile profile{};
                if (s_tracker->profile(profile) &&
                    (!s_published_any || !sameProfile(profile, s_last_published))) {
                    s_adapter->publish(profile);
                    s_last_published = profile;
                    s_published_any = true;
                }
                // D3/§8 persist cadence: every kPersistIntervalMs since
                // the last attempt, plus the valid false→true moment. A
                // failed put is retried at the next interval (§10) and
                // never blocks learning; attempt time advances either
                // way so a dying store cannot turn the pump into a
                // 1 Hz flash writer.
                if (result.valid_transition ||
                    (now - s_last_persist_ms) >= bw::baseline::kPersistIntervalMs) {
                    s_last_persist_ms = now;
                    if (s_store != nullptr) {
                        const bw::Result<void> saved =
                            bw::baseline::BaselineSerializer::saveToStore(s_store,
                                                                          s_tracker->state());
                        if (!saved.ok()) {
                            // §10: stay in memory until the next interval.
                        }
                    }
                }
            }
        }
    }
}

#if defined(ARDUINO)
void pumpTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        pumpOnce(ctx);
        app::tasks::completeCycle(bw::diag::TaskSlot::Baseline, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kBaselinePumpPeriodMs));
    }
}
#endif  // ARDUINO

}  // namespace f13
