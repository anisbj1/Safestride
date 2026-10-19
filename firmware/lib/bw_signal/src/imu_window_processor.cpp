#include <bw/signal/imu_window_processor.h>

#include <cmath>

namespace bw::signal {

void ImuWindowProcessor::feed(const ImuSample& sample) {
    // Inter-sample gap (also spans the previous window's tail): a gap beyond
    // kMaxSampleGapMs marks the frame Degraded at close() (spec §10).
    TimeMs gap_ms = 0;
    if (have_last_fed_t_ && sample.t_ms > last_fed_t_ms_) {
        gap_ms = sample.t_ms - last_fed_t_ms_;
        if (gap_ms > frame_max_gap_ms_) {
            frame_max_gap_ms_ = gap_ms;
        }
    }

    const float ax = sample.accel_g[0];
    const float ay = sample.accel_g[1];
    const float az = sample.accel_g[2];
    const float mag = std::sqrt(ax * ax + ay * ay + az * az);

    // ENMO: standard actigraphy metric, mean over the frame (F02 D1).
    const float enmo = mag > kStandardGravityG ? mag - kStandardGravityG : 0.0f;
    frame_enmo_sum_g_ += enmo;

    // Welford: numerically stable std without buffering the frame (F02 D6).
    ++frame_count_;
    const float d = mag - frame_mag_mean_g_;
    frame_mag_mean_g_ += d / static_cast<float>(frame_count_);
    frame_mag_m2_g_ += d * (mag - frame_mag_mean_g_);

    frame_gyro_sum_dps_2_ += sample.gyro_dps[0] * sample.gyro_dps[0] +
                             sample.gyro_dps[1] * sample.gyro_dps[1] +
                             sample.gyro_dps[2] * sample.gyro_dps[2];

    // Jerk uses the NOMINAL sample period, not the timestamp delta: sensor
    // jitter must not leak into the derivative [DERIVED] (F02 D6).
    if (have_prev_sample_ && gap_ms <= kMaxSampleGapMs) {
        const float dt_s =
            static_cast<float>(kImuSamplePeriodMs) / static_cast<float>(kMsPerSecond);
        const float dax = (ax - prev_sample_.accel_g[0]) / dt_s;
        const float day = (ay - prev_sample_.accel_g[1]) / dt_s;
        const float daz = (az - prev_sample_.accel_g[2]) / dt_s;
        frame_jerk_sum_g_per_s_2_ += dax * dax + day * day + daz * daz;
        ++frame_jerk_count_;
    }
    // Across a gap there is no meaningful derivative — skip (condition above).

    // Stillness with hysteresis (F02 D5): enter below the enter threshold,
    // leave only above the exit threshold, so noise cannot flap the state.
    if (!in_stillness_) {
        if (enmo < kStillnessEnmoThresholdG) {
            in_stillness_ = true;
            stillness_start_ms_ = sample.t_ms;
        }
    } else if (enmo > kStillnessExitEnmoThresholdG) {
        in_stillness_ = false;
    }

    // Rolling magnitude history for the dominant-frequency autocorrelation.
    // Overwriting the oldest entry IS the window — not a data loss (F02 D5).
    (void)mag_history_.push(mag);

    gravity_.update(sample.accel_g);

    prev_sample_ = sample;
    have_prev_sample_ = true;
    last_fed_t_ms_ = sample.t_ms;
    have_last_fed_t_ = true;
}

void ImuWindowProcessor::markFrameAnomaly() {
    frame_anomaly_ = true;
}

void ImuWindowProcessor::markGapBrokenContinuity() {
    // F02 D5: a gap/anomaly breaks continuous stillness (a dead sensor must
    // not report ever-growing stillness) and invalidates the magnitude
    // history ordering. Gravity is kept (orientation prior).
    in_stillness_ = false;
    stillness_start_ms_ = 0;
    mag_history_.clear();
}

void ImuWindowProcessor::resetFrame() {
    frame_count_ = 0;
    frame_jerk_count_ = 0;
    frame_enmo_sum_g_ = 0.0f;
    frame_mag_mean_g_ = 0.0f;
    frame_mag_m2_g_ = 0.0f;
    frame_jerk_sum_g_per_s_2_ = 0.0f;
    frame_gyro_sum_dps_2_ = 0.0f;
    frame_max_gap_ms_ = 0;
    frame_anomaly_ = false;
}

MotionFeatures ImuWindowProcessor::close(TimeMs window_end_ms) {
    // Tail gap: silence from the last fed sample to the window end (catches
    // a sensor that died mid-frame; a live stream has ~one sample period).
    TimeMs max_gap_ms = frame_max_gap_ms_;
    if (have_last_fed_t_ && window_end_ms > last_fed_t_ms_) {
        const TimeMs tail_ms = window_end_ms - last_fed_t_ms_;
        if (tail_ms > max_gap_ms) {
            max_gap_ms = tail_ms;
        }
    }

    const float coverage = static_cast<float>(frame_count_) *
                           static_cast<float>(kImuSamplePeriodMs) /
                           static_cast<float>(kMotionWindowMs);

    MotionFeatures out{};
    out.t_ms = window_end_ms;

    // Worst rule wins (F02 D6): too few samples -> Insufficient, gap/anomaly
    // -> Degraded, else Good. "No reliable data" beats a guess (00 §1).
    if (coverage < kMinWindowCoverageFrac) {
        out.quality = DataQuality::Insufficient;
    } else if (max_gap_ms > kMaxSampleGapMs || frame_anomaly_) {
        out.quality = DataQuality::Degraded;
    } else {
        out.quality = DataQuality::Good;
    }

    if (out.quality != DataQuality::Good) {
        markGapBrokenContinuity();
    }

    if (out.quality != DataQuality::Insufficient && frame_count_ > 0) {
        out.enmo_g = frame_enmo_sum_g_ / static_cast<float>(frame_count_);
        out.mag_std_g = frame_count_ >= 2
                            ? std::sqrt(frame_mag_m2_g_ / static_cast<float>(frame_count_))
                            : 0.0f;
        out.jerk_rms_g_per_s =
            frame_jerk_count_ > 0
                ? std::sqrt(frame_jerk_sum_g_per_s_2_ / static_cast<float>(frame_jerk_count_))
                : 0.0f;
        out.gyro_rms_dps = std::sqrt(frame_gyro_sum_dps_2_ / static_cast<float>(frame_count_));
        out.gravity_tilt_deg = gravity_.initialized() ? gravity_.tiltDeg() : 0.0f;
        // Continuous stillness: valid only while quality is Good (F02 D5);
        // non-Good already reset the state above, so the check is defensive.
        // Saturating cast (integrator 2026-10-04): TimeMs is 64-bit but the
        // contract field is uint32 — clamp at 0xFFFFFFFF instead of wrapping
        // after ~49.7 days of continuous stillness, where a wrapped value
        // would read as "not still" (no test: reaching the boundary needs
        // 4.3M contiguous windows; the clamp is arithmetic-only).
        const auto still_ms = window_end_ms - stillness_start_ms_;
        out.stillness_ms =
            (out.quality == DataQuality::Good && in_stillness_)
                ? static_cast<uint32_t>(still_ms > 0xFFFFFFFFull ? 0xFFFFFFFFull : still_ms)
                : 0;
        out.dominant_freq_hz = out.quality == DataQuality::Good ? dominantFreqHz() : 0.0f;
    }
    // Insufficient: every metric stays 0 with quality carrying the meaning
    // (02 §1: availability is expressed by quality/validity, not by a value).

    resetFrame();
    return out;
}

float ImuWindowProcessor::dominantFreqHz() const {
    if (mag_history_.size() < kMinSamplesForDominantFreq) {
        return 0.0f;  // history not full yet: "0 if none" (02 §4.2), F02 D5
    }
    constexpr size_t n = kMinSamplesForDominantFreq;

    float mean = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        mean += *mag_history_.at(i);
    }
    mean /= static_cast<float>(n);

    const float fs_hz = static_cast<float>(kMsPerSecond) / static_cast<float>(kImuSamplePeriodMs);
    int lag_min = static_cast<int>(std::ceil(fs_hz / kDominantFreqMaxHz));
    int lag_max = static_cast<int>(std::floor(fs_hz / kDominantFreqMinHz));
    if (lag_min < 1) {
        lag_min = 1;
    }
    if (lag_max > static_cast<int>(n) - 1) {
        lag_max = static_cast<int>(n) - 1;
    }
    if (lag_max < lag_min) {
        return 0.0f;
    }

    float r0 = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float d = *mag_history_.at(i) - mean;
        r0 += d * d;
    }
    if (!(r0 > 0.0f)) {
        return 0.0f;  // constant magnitude: no periodicity to report
    }

    float best_r_hat = 0.0f;
    int best_lag = 0;
    for (int lag = lag_min; lag <= lag_max; ++lag) {
        const size_t terms = n - static_cast<size_t>(lag);
        float r = 0.0f;
        for (size_t i = 0; i < terms; ++i) {
            r += (*mag_history_.at(i) - mean) * (*mag_history_.at(i + lag) - mean);
        }
        // Divide by the number of terms: the raw sum alone has a small-lag
        // bias (lag 3 averages 196 terms, lag 33 only 167), which let lag 3
        // win even when the true period was 33 samples — observed and fixed
        // via test_imu_dominant_frequency_from_sine [DERIVED].
        const float r_hat = r / static_cast<float>(terms);
        // Near-ties at multiples of the period (lag T, 2T, ...) must NOT
        // switch the winner: keep the shortest lag = true period, or the
        // frequency is reported as a subharmonic (F02 signal_config.h).
        if (r_hat > best_r_hat * (1.0f + kAutocorrTieMargin)) {
            best_r_hat = r_hat;
            best_lag = lag;
        }
    }

    const float r0_hat = r0 / static_cast<float>(n);
    const float strength = best_r_hat / r0_hat;
    if (best_lag <= 0 || strength < kMinAutocorrPeakStrength) {
        return 0.0f;  // no real periodicity (F02 D5)
    }
    return fs_hz / static_cast<float>(best_lag);
}

}  // namespace bw::signal
