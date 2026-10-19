#pragma once

// bw/signal/imu_window_processor.h — F02 §7: window metrics for MotionFeatures.
//
// Streaming side (persists across windows, F02 D5): gravity low-pass, stillness
// state machine with hysteresis, magnitude history for the dominant-frequency
// autocorrelation, jerk previous-sample state.
// Frame side (reset per close()): ENMO/std/jerk/gyro accumulators, gap and
// anomaly flags.
//
// feed() must only receive samples with t_ms < the current window end
// (routing is SignalProcessor's job, F02 D4) and in timestamp order — a
// non-monotonic sample must be rejected upstream (markFrameAnomaly()).

#include <bw/containers/ring_buffer.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/signal/gravity_estimator.h>
#include <bw/signal/signal_config.h>
#include <bw/types.h>

namespace bw::signal {

class ImuWindowProcessor {
public:
    // Processes one in-order sample of the current frame.
    void feed(const ImuSample& sample);

    // Call instead of feed() when a sample was dropped (backwards timestamp
    // or staging overflow): the frame cannot be trusted beyond a gap/anomaly.
    void markFrameAnomaly();

    // Finalizes MotionFeatures for the frame ending at window_end_ms and
    // resets the frame side. Streaming state follows the rules in F02 D5:
    // stillness and magnitude history reset on any quality < Good (continuity
    // broken), gravity is kept.
    MotionFeatures close(TimeMs window_end_ms);

    // True while the stillness state machine is in the "still" state (for
    // tests/debugging).
    bool inStillness() const { return in_stillness_; }

private:
    // Frame accumulators (reset in close()).
    uint32_t frame_count_ = 0;
    uint32_t frame_jerk_count_ = 0;
    float frame_enmo_sum_g_ = 0.0f;
    float frame_mag_mean_g_ = 0.0f;  // Welford mean of |a|
    float frame_mag_m2_g_ = 0.0f;    // Welford M2 of |a|
    float frame_jerk_sum_g_per_s_2_ = 0.0f;
    float frame_gyro_sum_dps_2_ = 0.0f;
    TimeMs frame_max_gap_ms_ = 0;
    bool frame_anomaly_ = false;

    // Streaming state (persists across windows).
    GravityEstimator gravity_;
    RingBuffer<float, kMinSamplesForDominantFreq> mag_history_;
    bool in_stillness_ = false;
    TimeMs stillness_start_ms_ = 0;
    ImuSample prev_sample_{};
    bool have_prev_sample_ = false;
    TimeMs last_fed_t_ms_ = 0;
    bool have_last_fed_t_ = false;

    // Computes dominant frequency from the magnitude history via
    // autocorrelation; returns 0 when there is no usable periodicity.
    float dominantFreqHz() const;
    void resetFrame();
    void markGapBrokenContinuity();  // stillness -> 0, history cleared (D5)
};

}  // namespace bw::signal
