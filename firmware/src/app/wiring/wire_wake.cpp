// firmware/src/app/wiring/wire_wake.cpp — F06's construction only.

#include "wire_wake.h"
#include "task_health.h"

#include "../app.h"  // AppContext: ctx.clock is read here, at construction
#include <bw/wake/wake_telemetry.h>

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for the pump (device build only)
#endif

namespace f06 {

namespace {

// Set by wire(); read by estimator(). Kept separate from the object so the
// accessor is usable before the guard variable of a function-local static
// would otherwise be touched from another translation unit.
bw::wake::WakefulnessEstimator* s_estimator = nullptr;

}  // namespace

#if defined(ARDUINO)

void pumpTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil). Equal periods do not guarantee producer/consumer
    // ordering: preserve F02's window-end stamp and measure age at consumption.
    TickType_t last_wake = xTaskGetTickCount();

    // Motion sequence of the last window folded in (0 = nothing yet) and
    // the epoch stamp of the last answer forwarded to the mailbox.
    uint32_t seen_motion_seq = 0;
    bw::TimeMs last_published_epoch_ms = 0;
    bool published_any = false;
    bw::TimeMs last_motion_ms = 0;
    bw::TimeMs previous_sample_ms = 0;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // No clock yet: honest wait (F02/F07 rule) — an epoch timeline
        // needs real time, never a fabricated t = 0.
        if (ctx.clock != nullptr) {
            const uint32_t seq = ctx.motion_features.sequence();
            if (seq != seen_motion_seq) {
                seen_motion_seq = seq;

                bw::MotionFeatures motion{};
                if (ctx.motion_features.get(motion)) {
                    // F06 pump contract (wire_wake.h): the IMU window is
                    // mandatory (an epoch without motion is fabricated
                    // low-motion evidence); PPG and activity are optional
                    // and are marked absent through their own contracts,
                    // never left as zeroed "Good" defaults.
                    bw::wake::WakeSample sample{};
                    sample.t_ms = ctx.clock->nowMs();
                    sample.motion = motion;
                    sample.quality =
                        bw::SourceQuality{motion.quality, bw::DataQuality::Insufficient};

                    bw::PpgFeatures ppg{};
                    if (seq == ctx.ppg_features.sequence() && ctx.ppg_features.get(ppg)) {
                        sample.ppg = ppg;
                        sample.quality.ppg = ppg.quality;
                    } else {
                        // Absent/stalled PPG: keep the zeroed struct but
                        // force its quality, so the assembler reads "no
                        // data" instead of a fabricated Good + 0 bpm.
                        sample.ppg.quality = bw::DataQuality::Insufficient;
                    }

                    bw::ActivityEstimate activity{};
                    if (ctx.activity_estimate.get(activity)) {
                        sample.activity = activity;
                    }  // else the zeroed Unknown/0 estimate is refused by
                       // the estimator (F06 §5) — the honest "no activity".

                    bw::TimeMs wake_check_ms = 0;
                    const bool has_wake_check = ctx.wake_check_passed_ms.get(wake_check_ms);
                    bw::WearEstimate wear{};
                    const bool has_wear = ctx.wear_estimate.get(wear);
                    const bw::wake::WakeEstimate est = estimator().step(
                        sample, has_wake_check, wake_check_ms, has_wear ? &wear : nullptr);
                    // Forward every NEW answer/evidence window, valid or not.
                    // CR-034 motion timestamps advance independently of epoch
                    // probabilities; never suppress new motion inside an epoch.
                    if (!published_any || est.probs.t_ms != last_published_epoch_ms ||
                        est.probs.wake_motion.t_ms != last_motion_ms) {
                        published_any = true;
                        last_published_epoch_ms = est.probs.t_ms;
                        last_motion_ms = est.probs.wake_motion.t_ms;
                        ctx.alertness_probabilities.set(est.probs);
                    }
                    const auto enmo = bw::wake::telemetryMicroG(motion.enmo_g);
                    const auto threshold =
                        bw::wake::telemetryMicroG(bw::wake::kDemoAwakeMotionEnmoG);
                    const bool telemetry_valid =
                        est.probs.wake_motion.valid && enmo.ok() && threshold.ok();
                    // Integer-only printf: float formatting overflowed F06Pump's
                    // original 2 KB stack on hardware (stack-canary panic).
                    Serial.printf(
                        "[WAKEMOTION] t=%llu valid=%u qualifying=%u enmo_g=%lu.%06lu "
                        "threshold_g=%lu.%06lu\n",
                        static_cast<unsigned long long>(motion.t_ms), telemetry_valid,
                        telemetry_valid && est.probs.wake_motion.qualifying,
                        static_cast<unsigned long>(enmo.value / bw::wake::kTelemetryMicroGPerG),
                        static_cast<unsigned long>(enmo.value % bw::wake::kTelemetryMicroGPerG),
                        static_cast<unsigned long>(threshold.value /
                                                   bw::wake::kTelemetryMicroGPerG),
                        static_cast<unsigned long>(threshold.value %
                                                   bw::wake::kTelemetryMicroGPerG));
                    // Separate line keeps the existing WAKEMOTION parser stable.
                    // Ages are meaningful only when the corresponding ordered /
                    // present flag is set; future timestamps never underflow.
                    Serial.printf(
                        "[WAKEGATE] sample_ms=%llu motion_age_ms=%llu motion_ordered=%u "
                        "wear_present=%u wear_ms=%llu wear_age_ms=%llu wear_ordered=%u "
                        "cadence_ms=%llu previous_present=%u imu_q=%u motion_q=%u "
                        "reject=%s estimate_valid=%u\n",
                        static_cast<unsigned long long>(sample.t_ms),
                        static_cast<unsigned long long>(
                            sample.t_ms >= motion.t_ms ? sample.t_ms - motion.t_ms : 0),
                        sample.t_ms >= motion.t_ms, has_wear,
                        static_cast<unsigned long long>(wear.t_ms),
                        static_cast<unsigned long long>(
                            has_wear && sample.t_ms >= wear.t_ms ? sample.t_ms - wear.t_ms : 0),
                        has_wear && sample.t_ms >= wear.t_ms,
                        static_cast<unsigned long long>(previous_sample_ms != 0 &&
                                                                sample.t_ms >= previous_sample_ms
                                                            ? sample.t_ms - previous_sample_ms
                                                            : 0),
                        previous_sample_ms != 0, static_cast<unsigned>(sample.quality.imu),
                        static_cast<unsigned>(motion.quality),
                        bw::wake::inputRejectionName(estimator().lastInputRejection()),
                        est.probs.valid);
                    previous_sample_ms = sample.t_ms;
                }
            }
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Wake, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kWakePumpPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx, const bw::IBaselineProvider* baseline) {
    // Function-local static: constructed on the first call only, so a second
    // wire() keeps the first instance (idempotence contract above) and no
    // dynamic allocation happens after startup (00 §7.2). The clock pointer
    // is read here, after F01's wire() has filled it (README call order).
    static bw::wake::WakefulnessEstimator instance{
        bw::wake::WakeConfig{},
        ctx.clock,  // may still be nullptr before M1: time-of-day term off
        baseline,   // F13 IBaselineProvider injected at P4 (W6); nullptr = absent (honest)
        nullptr,    // F05 IModel: arrives with M5
    };
    s_estimator = &instance;
    // Task table landed at P4 (2026-10-12): create the 1 Hz pump that
    // folds feature windows into epoch estimates. Before F02 publishes the
    // pump just waits — an empty channel is an ordinary state.
#if defined(ARDUINO)
    app::tasks::checkCreated(xTaskCreatePinnedToCore(pumpTask, "F06Pump", kWakePumpStackBytes, &ctx,
                                                     kWakePumpPriority, nullptr, kWakePumpCore),
                             bw::diag::TaskSlot::Wake, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::wake::WakefulnessEstimator& estimator() {
    return *s_estimator;
}

}  // namespace f06
