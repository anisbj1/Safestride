#pragma once

// bw/signal/signal_processor.h — F02 feature entry point: turns raw
// ImuSample/PpgSample streams into one MotionFeatures + PpgFeatures pair per
// 1 s window (F02 §1).
//
// Design (F02 D4): feed() stages samples timestamp-ordered in SpscQueues and
// notes window boundaries; step(now_ms, out) closes AT MOST ONE window per
// call — either because a sample crossed the boundary or because the stream
// stalled past window_end + kMaxSampleGapMs (spec §10). Callers catch up
// after a delayed task with `while (proc.step(now, out)) publish(out);`.
// One sample per stream may be parked in a carry slot so nothing that
// belongs to the next window is lost or reordered.
//
// Threading contract: feed*() and step() are called from ONE task (the
// ProcessingTask, 01 §3.2) — the SpscQueues allow a single producer and a
// single consumer, which this class itself straddles only when the caller
// serializes its calls. The class never reads a clock: `now` comes in as an
// argument (host-pure, F02 D4).

#include <bw/containers/ring_buffer.h>
#include <bw/containers/spsc_queue.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/signal/feature_window_scheduler.h>
#include <bw/signal/heart_rate_estimator.h>
#include <bw/signal/imu_window_processor.h>
#include <bw/signal/ppg_beat_detector.h>
#include <bw/signal/ppg_filter_chain.h>
#include <bw/signal/ppg_quality_estimator.h>
#include <bw/signal/signal_config.h>
#include <bw/signal/tester_heart_rate.h>
#include <bw/types.h>

namespace bw::signal {

struct SignalWindowOutput {
    TimeMs window_end_ms = 0;
    bool has_motion = false;
    MotionFeatures motion{};
    bool has_ppg = false;
    PpgFeatures ppg{};

    // Per-window PPG raw diagnostics for bench observability (F02 FIX
    // 2026-10-14, the [PPGWIN] line in wire_f02): mirrored from PpgFrameStats
    // / HeartRateResult at close so the composition root prints per-window
    // truth without reaching into processor internals. Valid whenever
    // has_ppg; raw_ir_min/max are meaningful only when ppg_sample_count > 0.
    uint32_t ppg_sample_count = 0;        // raw samples drained into the window
    uint32_t ppg_beat_count = 0;          // beats inside the kHrWindowMs lookback
    float ppg_saturated_fraction = 0.0f;  // fraction of samples at ADC full scale
    uint32_t ppg_raw_ir_min = 0;          // raw IR counts min (counts, 18-bit)
    uint32_t ppg_raw_ir_max = 0;          // raw IR counts max; 0 span = flatline

    // FIX-2 in-band observability ([PPGPK] line): one capture answers "is a
    // pulse present in the filtered band at all?" — present: strongest_peak
    // >> threshold >> filtered_std and peaks_above > 0; absent (drift/noise):
    // all three comparable (drift also gives candidates ~ 0, band-limited
    // noise gives candidates ~ n/2). Counters mirror the beat detector's
    // own tallies (one rule, one threshold).
    float ppg_filtered_std = 0.0f;           // std of the filtered stream [counts]
    float ppg_strongest_peak = 0.0f;         // tallest local peak [counts]
    float ppg_peak_threshold = 0.0f;         // adaptive threshold at last sample
    uint16_t ppg_candidate_peaks = 0;        // local maxima in the window
    uint16_t ppg_peaks_above_threshold = 0;  // ... taller than the threshold
};

class SignalProcessor {
public:
    explicit SignalProcessor(PpgProfile profile = PpgProfile::Filtered100Hz)
        : profile_(profile), filter_(profile) {}
    // Queues `count` samples (may be 0; nullptr ignored). Non-monotonic
    // samples are dropped and mark the frame anomalous (F02 D4).
    void feedImu(const ImuSample* samples, size_t count);
    void feedPpg(const PpgSample* samples, size_t count);

    // Closes at most one window; returns true when `out` was filled.
    bool step(TimeMs now_ms, SignalWindowOutput& out);

    // Closes all windows currently due, retaining only the newest result.
    // `step()` remains one-window-per-call so callers needing per-window
    // handling can continue to use it directly.
    bool stepUntilIdle(TimeMs now_ms, SignalWindowOutput& latest);

    // F28 debug tap (spec §3): filtered PPG points, oldest-first.
    size_t copyFilteredPpg(PpgFilteredPoint* out, size_t max) const;

private:
    struct MotionLookbackEntry {
        float enmo_g;
        bool known;  // false = IMU was Insufficient that window (F02 D6)
    };

    void processPpgSample(const PpgSample& sample);
    void drainImu(TimeMs window_end_ms);
    void drainPpg(TimeMs window_end_ms);
    void closeWindow(SignalWindowOutput& out);

    FeatureWindowScheduler scheduler_;
    SpscQueue<ImuSample, kImuStagingCapacity> imu_staging_;
    SpscQueue<PpgSample, kPpgStagingCapacity> ppg_staging_;

    // One parked sample per stream (first sample that belongs to a LATER
    // window, F02 D4).
    ImuSample imu_carry_{};
    bool imu_carry_pending_ = false;
    PpgSample ppg_carry_{};
    bool ppg_carry_pending_ = false;

    // Feed-side monotonicity guards (per stream).
    TimeMs last_fed_imu_t_ms_ = 0;
    bool have_fed_imu_t_ = false;
    TimeMs last_fed_ppg_t_ms_ = 0;
    bool have_fed_ppg_t_ = false;

    // hr_valid lifetime policy (FIX loop 5, bench round 7): hold state
    // across windows. hr_hold_active_ = a fresh-valid window latched the
    // hold; hr_hold_started_ + hr_hold_since_ms_ stamp the FIRST window of
    // the current fresh-invalid run (the "consecutive implausible stretch"
    // clock of kHrValidDropoutMs). Released by the beat-gap rule (pulse
    // stopped — takes precedence) or when the stretch outlasts
    // kHrValidDropoutMs. Fixed state, zero-initialized, no heap.
    bool hr_hold_active_ = false;
    bool hr_hold_started_ = false;
    TimeMs hr_hold_since_ms_ = 0;

    // Processing-side state.
    PpgProfile profile_;
    ImuWindowProcessor imu_;
    PpgFilterChain filter_;
    PpgBeatDetector beat_detector_;
    TesterHeartRate tester_hr_;
    PpgFrameStats ppg_stats_;
    TimeMs last_processed_ppg_t_ms_ = 0;
    bool have_processed_ppg_t_ = false;
    RingBuffer<MotionLookbackEntry, kHrMotionLookbackWindows> motion_lookback_;
};

}  // namespace bw::signal
