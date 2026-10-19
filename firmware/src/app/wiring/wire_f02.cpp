#include "wire_f02.h"
#include "task_health.h"

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for ProcessingTask (device build only)

#include "wire_research.h"  // F28 A5 gate: [PPGWIN] mutes while the raw tee runs
                            // (same predicate as app.cpp's [BENCH]/[LOOP] lines)
#endif

namespace f02 {

namespace {

// Static-storage processor: constructed before any task runs - no heap, no
// init-order dependency (embedded conventions). Everything configurable
// (windows, thresholds, filters) is compile-time configuration in
// bw/signal/signal_config.h (F02 D9). The explicit 25 Hz profile matches
// F01's 100 raw sps / four-sample FIFO averaging, verified against the tester.
bw::signal::SignalProcessor s_processor{bw::signal::PpgProfile::Tester25Hz};

#if defined(ARDUINO)

// Bench-formatting helpers for the [PPGWIN] line (F02 FIX observability,
// 2026-10-14): device-only, static strings, no String, no heap.
const char* ppgQualityName(bw::DataQuality q) {
    switch (q) {
    case bw::DataQuality::Good:
        return "Good";
    case bw::DataQuality::Degraded:
        return "Degraded";
    case bw::DataQuality::Insufficient:
        return "Insufficient";
    }
    return "?";
}

// One compact per-window PPG stats line (1 s cadence — one per closed
// window), the required observability deliverable for the finger round.
// Numbers: dc/min/max = raw IR counts (18-bit full scale 262143; min == max
// == 262143 = ADC railed, min == max below = flat signal); pi = perfusion
// AC/DC; sat = fraction at full scale; n = samples in window (≈100 at
// 25 Hz in the tester profile — low = drain/timestamp problem, not signal); b = beats in the
// 10 s HR lookback; sqi/q = quality gate inputs; hr = hr_valid, bpm printed
// Only when valid. Gated exactly like app.cpp's bench lines (A5): muted
// while the F28 raw tee is active; with the capture flag OFF (repo
// default) rawTee() is always null, so the line prints every window.
//
// Companion [PPGPK] line (F02 FIX-2, same gate, printed right after): the
// in-band discrimination that ended the blind rounds — fstd = std of the
// filtered stream, fpeak = tallest local peak, thr = adaptive peak
// threshold in force, npk = local peaks counted, nab = peaks above thr
// (all counts at the current ADC range). Reading: a real ~1 Hz pulse shows
// fpeak >> thr >> fstd with nab ≥ beats; drift shows npk ≈ 0 with fstd
// comparable to fpeak; band-limited noise shows npk ≈ n/2 with fpeak ~
// 2–3 × fstd. Decides detector-vs-optics from ONE capture.
void printPpgWindow(const bw::signal::SignalWindowOutput& w) {
    if (::f28::rawTee() != nullptr) {
        return;  // A5: never tear capture rows
    }
    const bw::PpgFeatures& p = w.ppg;
    Serial.printf(
        "[PPGWIN] t=%llu dc=%lu min=%lu max=%lu pi=%.4f sat=%.2f n=%u b=%u sqi=%.2f "
        "q=%s hr=%d bpm=%.1f\n",
        static_cast<unsigned long long>(w.window_end_ms), static_cast<unsigned long>(p.ir_dc_level),
        static_cast<unsigned long>(w.ppg_raw_ir_min), static_cast<unsigned long>(w.ppg_raw_ir_max),
        static_cast<double>(p.perfusion_index), static_cast<double>(w.ppg_saturated_fraction),
        static_cast<unsigned int>(w.ppg_sample_count), static_cast<unsigned int>(w.ppg_beat_count),
        static_cast<double>(p.sqi), ppgQualityName(p.quality), p.hr_valid ? 1 : 0,
        static_cast<double>(p.hr_bpm));
    Serial.printf(
        "[PPGPK] t=%llu fstd=%.1f fpeak=%.1f thr=%.1f npk=%u nab=%u\n",
        static_cast<unsigned long long>(w.window_end_ms), static_cast<double>(w.ppg_filtered_std),
        static_cast<double>(w.ppg_strongest_peak), static_cast<double>(w.ppg_peak_threshold),
        static_cast<unsigned int>(w.ppg_candidate_peaks),
        static_cast<unsigned int>(w.ppg_peaks_above_threshold));
}

#endif  // ARDUINO

}  // namespace

#if defined(ARDUINO)

void processingTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md 3.2: no delay(), use
    // vTaskDelayUntil): window closes stay on the 1 s grid even if one tick
    // is late, so the F03 consumer observes a constant 1 Hz publish phase.
    TickType_t last_wake = xTaskGetTickCount();

    bw::signal::SignalWindowOutput window{};
    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // Drain both D10 queues completely before stepping: the queues hold
        // one consumer period (64 = 1 s at 50 Hz + jitter, app.h), so this
        // loop is bounded and runs in microseconds. A full queue upstream
        // already showed up as SpscQueue::drops() - never our problem to
        // block on (CR-004).
        bw::ImuSample imu{};
        while (ctx.imu_samples.pop(imu)) {
            s_processor.feedImu(&imu, 1);
        }
        bw::PpgSample ppg{};
        while (ctx.ppg_samples.pop(ppg)) {
            s_processor.feedPpg(&ppg, 1);
        }

        // Window bookkeeping time comes from ctx.clock (IClock is read by the
        // caller, never inside the module - F02 D4). No clock yet: now = 0 is
        // harmless, step() simply closes nothing until time exists.
        const bw::TimeMs now = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
        if (s_processor.stepUntilIdle(now, window)) {
            if (window.has_motion) {
                ctx.motion_features.set(window.motion);
            }
            if (window.has_ppg) {
                ctx.ppg_features.set(window.ppg);
                // F02 FIX observability: one [PPGWIN] stats line per closed
                // window (A5-gated inside — muted while the raw tee runs).
                printPpgWindow(window);
            }
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Processing, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kProcessingTaskPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction only - s_processor already exists at file scope. The two
    // D10 queues in AppContext are drained by ProcessingTask (pop -> feedImu/
    // feedPpg) and window bookkeeping time comes from ctx.clock via
    // step(now_ms, ...): the processor itself stays pure (F02 D4 - IClock is
    // read by the caller, never inside the module).
    //
    // Task table landed 2026-10-08: wire() also creates ProcessingTask, per
    // the wiring README ("construction, injection and task creation"). It can
    // start before F03's pump exists - an empty mailbox read is a tested
    // state (LatestValue::get returns false), and the queues are safe to
    // drain the moment F01's SensorTask starts pushing.
#if defined(ARDUINO)
    app::tasks::checkCreated(
        xTaskCreatePinnedToCore(processingTask, "ProcessingTask", kProcessingTaskStackBytes, &ctx,
                                kProcessingTaskPriority, nullptr, kProcessingTaskCore),
        bw::diag::TaskSlot::Processing, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::signal::SignalProcessor& processor() {
    return s_processor;
}

}  // namespace f02
