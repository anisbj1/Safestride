// F02 module tests: gravity, window scheduler, IMU window metrics, PPG
// filter, beat detection, heart-rate estimation, quality evaluation.

#include <cmath>

#include <unity.h>

#include <bw/signal/feature_window_scheduler.h>
#include <bw/signal/gravity_estimator.h>
#include <bw/signal/heart_rate_estimator.h>
#include <bw/signal/imu_window_processor.h>
#include <bw/signal/ppg_beat_detector.h>
#include <bw/signal/ppg_filter_chain.h>
#include <bw/signal/ppg_quality_estimator.h>
#include <bw/signal/signal_config.h>

using bw::DataQuality;
using bw::ImuSample;
using bw::TimeMs;
namespace signal = bw::signal;

namespace {

constexpr float kTestPi = 3.14159265f;

ImuSample mkImu(TimeMs t, float ax, float ay, float az) {
    ImuSample s{};
    s.t_ms = t;
    s.accel_g[0] = ax;
    s.accel_g[1] = ay;
    s.accel_g[2] = az;
    s.gyro_dps[0] = 0.0f;
    s.gyro_dps[1] = 0.0f;
    s.gyro_dps[2] = 0.0f;
    return s;
}

// Feeds one full 1 s frame of a CONSTANT acceleration (50 samples, t < end).
void feedConstantFrame(signal::ImuWindowProcessor& imu, TimeMs start_ms, float ax, float ay,
                       float az) {
    for (uint32_t i = 0; i < signal::kMotionWindowMs / signal::kImuSamplePeriodMs; ++i) {
        imu.feed(mkImu(start_ms + i * signal::kImuSamplePeriodMs, ax, ay, az));
    }
}

}  // namespace

// ---- GravityEstimator ------------------------------------------------------

void test_gravity_seeds_from_first_sample() {
    signal::GravityEstimator g;
    TEST_ASSERT_FALSE(g.initialized());
    const float in[3] = {0.0f, 0.0f, 1.0f};
    g.update(in);
    TEST_ASSERT_TRUE(g.initialized());
    float out[3];
    g.vectorG(out);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, out[2]);
}

void test_gravity_tilt_six_orientations() {
    struct Case {
        float accel[3];
        float expect_deg;
    };
    // +z, +x, -x, +y, -y, -z — the six axis orientations (F02 §9).
    const Case cases[] = {
        {{0.0f, 0.0f, 1.0f}, 0.0f},  {{1.0f, 0.0f, 0.0f}, 90.0f},  {{-1.0f, 0.0f, 0.0f}, 90.0f},
        {{0.0f, 1.0f, 0.0f}, 90.0f}, {{0.0f, -1.0f, 0.0f}, 90.0f}, {{0.0f, 0.0f, -1.0f}, 180.0f},
    };
    for (const Case& c : cases) {
        signal::GravityEstimator g;
        for (int i = 0; i < 5; ++i) {
            g.update(c.accel);  // a few updates: seeding makes step 1 exact
        }
        TEST_ASSERT_FLOAT_WITHIN(0.5f, c.expect_deg, g.tiltDeg());
    }
}

void test_gravity_converges_on_slow_change() {
    signal::GravityEstimator g;
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float side[3] = {1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 50; ++i) {
        g.update(up);
    }
    // 500 updates at alpha = dt/(tau+dt) ≈ 0.0099: e^-5 ≈ 0.7 % residual.
    for (int i = 0; i < 500; ++i) {
        g.update(side);
    }
    float out[3];
    g.vectorG(out);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, out[0]);  // converged to +x
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, out[2]);  // old +z gone
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, g.tiltDeg());
}

// ---- FeatureWindowScheduler ------------------------------------------------

void test_scheduler_boundary_close() {
    signal::FeatureWindowScheduler s;
    TEST_ASSERT_EQUAL_UINT64(signal::kMotionWindowMs, s.windowEnd());
    s.noteSample(signal::kMotionWindowMs - 1);
    TEST_ASSERT_FALSE(s.boundaryPassed());
    s.noteSample(signal::kMotionWindowMs);  // first sample of the next window
    TEST_ASSERT_TRUE(s.boundaryPassed());
    TEST_ASSERT_TRUE(s.shouldClose(signal::kMotionWindowMs));  // before grace
}

void test_scheduler_stall_close_at_end_plus_grace() {
    signal::FeatureWindowScheduler s;
    TEST_ASSERT_FALSE(s.shouldClose(signal::kMotionWindowMs - 1));
    TEST_ASSERT_FALSE(s.shouldClose(signal::kMotionWindowMs + signal::kMaxSampleGapMs - 1));
    TEST_ASSERT_TRUE(s.shouldClose(signal::kMotionWindowMs + signal::kMaxSampleGapMs));
}

void test_scheduler_advance_clears_boundary() {
    signal::FeatureWindowScheduler s;
    s.noteSample(signal::kMotionWindowMs);
    s.advance();
    TEST_ASSERT_FALSE(s.boundaryPassed());
    TEST_ASSERT_EQUAL_UINT64(2 * signal::kMotionWindowMs, s.windowEnd());
    // Grace applies to the NEW window end.
    TEST_ASSERT_FALSE(s.shouldClose(signal::kMotionWindowMs + signal::kMaxSampleGapMs));
    TEST_ASSERT_TRUE(s.shouldClose(2 * signal::kMotionWindowMs + signal::kMaxSampleGapMs));
}

// ---- ImuWindowProcessor ----------------------------------------------------

void test_imu_constant_one_g_zero_enmo() {
    signal::ImuWindowProcessor imu;
    feedConstantFrame(imu, 0, 0.0f, 0.0f, 1.0f);
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Good), static_cast<int>(f.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.enmo_g);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.mag_std_g);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.jerk_rms_g_per_s);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.gyro_rms_dps);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, f.gravity_tilt_deg);
    // Constant 1 g means ENMO 0 from the first sample: stillness starts at
    // t = 0 and spans the whole window (F02 D5).
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(signal::kMotionWindowMs), f.stillness_ms);
    TEST_ASSERT_EQUAL_UINT64(signal::kMotionWindowMs, f.t_ms);
}

void test_imu_metrics_for_known_motion() {
    signal::ImuWindowProcessor imu;
    // Alternating |a| of 1.0 / 1.2 g at the nominal rate, gyro (3,4,0) dps.
    for (uint32_t i = 0; i < 50; ++i) {
        ImuSample s =
            mkImu(i * signal::kImuSamplePeriodMs, 0.0f, 0.0f, (i % 2u) == 0u ? 1.0f : 1.2f);
        s.gyro_dps[0] = 3.0f;
        s.gyro_dps[1] = 4.0f;
        imu.feed(s);
    }
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Good), static_cast<int>(f.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, f.enmo_g);     // mean of max(|a|-1,0)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, f.mag_std_g);  // std of {1.0, 1.2}
    // Jerk: |Δa| = 0.2 g over the nominal 20 ms ⇒ 10 g/s per step.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, f.jerk_rms_g_per_s);
    // Gyro RMS: sqrt(3² + 4²) = 5 dps.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, f.gyro_rms_dps);
    // Mean ENMO 0.1 g is above the stillness exit threshold ⇒ not still.
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);
}

void test_imu_stillness_enter_exit_hysteresis() {
    signal::ImuWindowProcessor imu;
    // Window 1: quiet — enters stillness at the first sample (t = 0).
    feedConstantFrame(imu, 0, 0.0f, 0.0f, 1.0f);
    bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_TRUE(imu.inStillness());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(signal::kMotionWindowMs), f.stillness_ms);

    // Window 2: ENMO 0.03 g — between enter (0.02) and exit (0.05): a still
    // state must NOT flap (hysteresis, F02 D5).
    feedConstantFrame(imu, signal::kMotionWindowMs, 0.0f, 0.0f, 1.03f);
    f = imu.close(2 * signal::kMotionWindowMs);
    TEST_ASSERT_TRUE(imu.inStillness());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(2 * signal::kMotionWindowMs), f.stillness_ms);

    // Window 3: ENMO 0.06 g — above the exit threshold: leaves stillness.
    feedConstantFrame(imu, 2 * signal::kMotionWindowMs, 0.0f, 0.0f, 1.06f);
    f = imu.close(3 * signal::kMotionWindowMs);
    TEST_ASSERT_FALSE(imu.inStillness());
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);
}

void test_imu_motion_does_not_enter_stillness() {
    signal::ImuWindowProcessor imu;
    feedConstantFrame(imu, 0, 0.0f, 0.0f, 1.03f);  // ENMO 0.03 > 0.02 enter
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_FALSE(imu.inStillness());
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);
}

void test_imu_gap_marks_degraded_and_resets_stillness() {
    signal::ImuWindowProcessor imu;
    // Quiet first half (enters stillness), 440 ms hole, quiet second half.
    for (uint32_t i = 0; i < 24; ++i) {
        imu.feed(mkImu(i * signal::kImuSamplePeriodMs, 0.0f, 0.0f, 1.0f));
    }
    for (TimeMs t = 900; t < 1000; t += signal::kImuSamplePeriodMs) {
        imu.feed(mkImu(t, 0.0f, 0.0f, 1.0f));
    }
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Degraded), static_cast<int>(f.quality));
    // A gap breaks continuous stillness — a dead sensor must never report
    // growing stillness (F02 D5).
    TEST_ASSERT_FALSE(imu.inStillness());
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);
}

void test_imu_no_samples_insufficient() {
    signal::ImuWindowProcessor imu;
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(f.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.enmo_g);
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.dominant_freq_hz);
}

void test_imu_partial_frame_below_coverage_insufficient() {
    signal::ImuWindowProcessor imu;
    // 10 samples = 200 ms of a 1000 ms window (20 % < 50 % coverage).
    for (uint32_t i = 0; i < 10; ++i) {
        imu.feed(mkImu(i * signal::kImuSamplePeriodMs, 0.0f, 0.0f, 1.0f));
    }
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(f.quality));
}

void test_imu_backwards_timestamp_marks_anomaly() {
    signal::ImuWindowProcessor imu;
    feedConstantFrame(imu, 0, 0.0f, 0.0f, 1.0f);
    imu.markFrameAnomaly();  // what feed() does with a non-monotonic sample
    const bw::MotionFeatures f = imu.close(signal::kMotionWindowMs);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Degraded), static_cast<int>(f.quality));
    TEST_ASSERT_EQUAL_UINT32(0, f.stillness_ms);  // continuity broken
}

void test_imu_dominant_frequency_from_sine() {
    signal::ImuWindowProcessor imu;
    // 5 s of a 1.5 Hz oscillation on the gravity axis: |a| itself
    // oscillates at 1.5 Hz (no frequency doubling on the z axis).
    const TimeMs end_ms = 5 * signal::kMotionWindowMs;
    for (TimeMs t = 0; t < end_ms; t += signal::kImuSamplePeriodMs) {
        const float phase = 2.0f * kTestPi * 1.5f * static_cast<float>(t) /
                            static_cast<float>(signal::kMsPerSecond);
        imu.feed(mkImu(t, 0.0f, 0.0f, 1.0f + 0.1f * std::sin(phase)));
    }
    const bw::MotionFeatures f = imu.close(end_ms);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Good), static_cast<int>(f.quality));
    // Lag quantization: best lag is round(50/1.5) = 33 ⇒ 1.515 Hz.
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 1.5f, f.dominant_freq_hz);
}

void test_imu_dominant_frequency_zero_when_constant() {
    signal::ImuWindowProcessor imu;
    for (TimeMs t = 0; t < 5 * signal::kMotionWindowMs; t += signal::kImuSamplePeriodMs) {
        imu.feed(mkImu(t, 0.0f, 0.0f, 1.0f));
    }
    const bw::MotionFeatures f = imu.close(5 * signal::kMotionWindowMs);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, f.dominant_freq_hz);
}

// ---- PpgFilterChain --------------------------------------------------------

void test_filter_constant_dc_settles_immediately() {
    signal::PpgFilterChain filter;
    // Steady-state priming (F02 D7): a constant input must come out as ~0
    // from sample ONE — no ~3 s DC-blocker startup transient.
    for (int i = 0; i < 100; ++i) {
        const float y = filter.push(50000);
        TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, y);
    }
}

void test_filter_removes_dc_and_keeps_1hz() {
    signal::PpgFilterChain filter;
    float sum = 0.0f;
    float peak = 0.0f;
    const int samples = 1000;  // 10 s at 100 Hz
    for (int i = 0; i < samples; ++i) {
        const float t_s = static_cast<float>(i) / 100.0f;
        const float x = 50000.0f + 1000.0f * std::sin(2.0f * kTestPi * 1.0f * t_s);
        const float y = filter.push(static_cast<uint32_t>(x));
        // Skip the first second (one DC-blocker time constant of settling).
        if (i >= 100) {
            sum += y;
            const float mag = std::fabs(y);
            if (mag > peak) {
                peak = mag;
            }
        }
    }
    const float mean = sum / static_cast<float>(samples - 100);
    // DC removed: residual < 0.1 % of the 50 000-count baseline.
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 0.0f, mean);
    // Cascade gain at 1 Hz ≈ 0.968 (tools/design_ppg_filter.py table).
    TEST_ASSERT_FLOAT_WITHIN(0.10f * 1000.0f, 0.968f * 1000.0f, peak);
}

void test_filter_corner_gain_half_at_0_5hz() {
    signal::PpgFilterChain filter;
    float peak = 0.0f;
    for (int i = 0; i < 600; ++i) {  // 6 s at 0.5 Hz
        const float t_s = static_cast<float>(i) / 100.0f;
        const float x = 50000.0f + 1000.0f * std::sin(2.0f * kTestPi * 0.5f * t_s);
        const float y = filter.push(static_cast<uint32_t>(x));
        if (i >= 300) {
            const float mag = std::fabs(y);
            if (mag > peak) {
                peak = mag;
            }
        }
    }
    // -3 dB corner: output amplitude ≈ 0.707 × input (Butterworth definition).
    TEST_ASSERT_FLOAT_WITHIN(0.08f * 1000.0f, 0.707f * 1000.0f, peak);
}

void test_filter_attenuates_out_of_band() {
    signal::PpgFilterChain filter;
    float peak = 0.0f;
    for (int i = 0; i < 500; ++i) {  // 8 Hz — well above the 4 Hz corner
        const float t_s = static_cast<float>(i) / 100.0f;
        const float x = 50000.0f + 1000.0f * std::sin(2.0f * kTestPi * 8.0f * t_s);
        const float y = filter.push(static_cast<uint32_t>(x));
        if (i >= 250) {
            const float mag = std::fabs(y);
            if (mag > peak) {
                peak = mag;
            }
        }
    }
    // |H(8 Hz)| ≈ 0.235 (script table): output must be under a quarter.
    TEST_ASSERT_TRUE(peak < 0.30f * 1000.0f);
}

void test_filter_debug_ring_is_rolling() {
    signal::PpgFilterChain filter;
    const uint32_t pushed = static_cast<uint32_t>(signal::kPpgFilteredDebugCapacity + 100);
    for (uint32_t i = 0; i < pushed; ++i) {
        (void)filter.push(i);
    }
    signal::PpgFilteredPoint buf[signal::kPpgFilteredDebugCapacity];
    const size_t n = filter.copyDebug(buf, signal::kPpgFilteredDebugCapacity);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(signal::kPpgFilteredDebugCapacity),
                             static_cast<uint32_t>(n));
    // Oldest point is the 100th pushed: ring overwrote the first 100 (D4).
    TEST_ASSERT_EQUAL_UINT32(100, buf[0].ir_raw);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(pushed - 1), buf[n - 1].ir_raw);
}

// ---- PpgBeatDetector -------------------------------------------------------

void test_beat_detector_counts_beats_on_sine() {
    signal::PpgBeatDetector detector;
    int detected = 0;
    for (int i = 0; i < 500; ++i) {  // 5 s of 1 Hz (60 bpm), 100 Hz
        const float t_s = static_cast<float>(i) / 100.0f;
        const float x = 1000.0f * std::sin(2.0f * kTestPi * 1.0f * t_s);
        if (detector.push(x, 50000, static_cast<TimeMs>(i) * 10)) {
            ++detected;
        }
    }
    TEST_ASSERT_EQUAL_INT(5, detected);  // one peak per period
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(detector.beats().size()));
}

void test_beat_detector_refractory_rejects_double() {
    signal::PpgBeatDetector detector;
    // Peak 1 near t = 30, peak 2 near t = 130 (100 ms apart < 300 ms
    // refractory) — the second must be rejected (dicrotic-notch rule).
    const float pulse1[] = {0.0f, 100.0f, 500.0f, 100.0f, 0.0f};
    const float pulse2[] = {0.0f, 100.0f, 500.0f, 100.0f, 0.0f};
    int detected = 0;
    TimeMs t = 0;
    for (float x : pulse1) {
        detected += detector.push(x, 50000, t) ? 1 : 0;
        t += signal::kPpgSamplePeriodMs;
    }
    t = 100;  // second pulse starts 60 ms after the first ended
    for (float x : pulse2) {
        detected += detector.push(x, 50000, t) ? 1 : 0;
        t += signal::kPpgSamplePeriodMs;
    }
    TEST_ASSERT_EQUAL_INT(1, detected);
    // A third pulse after the refractory must be accepted again.
    t = 500;
    for (float x : pulse2) {
        detected += detector.push(x, 50000, t) ? 1 : 0;
        t += signal::kPpgSamplePeriodMs;
    }
    TEST_ASSERT_EQUAL_INT(2, detected);
}

void test_beat_detector_floor_rejects_small_peaks() {
    signal::PpgBeatDetector detector;
    // Amplitude 40 counts at DC 50 000 ⇒ PI 0.08 % < the 0.1 % floor:
    // peaks below kMinPerfusionIndex × DC can never be trusted (F02 D6).
    int detected = 0;
    for (int i = 0; i < 300; ++i) {
        const float t_s = static_cast<float>(i) / 100.0f;
        const float x = 40.0f * std::sin(2.0f * kTestPi * 1.0f * t_s);
        detected += detector.push(x, 50000, static_cast<TimeMs>(i) * 10) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(0, detected);
}

// ---- HeartRateEstimator ----------------------------------------------------

void test_hr_median_from_regular_beats() {
    bw::RingBuffer<TimeMs, signal::kMaxBeatsInHrWindow> beats;
    for (int k = 0; k < 10; ++k) {
        beats.push(static_cast<TimeMs>(500 + k * 857));  // ~70 bpm
    }
    const signal::HeartRateResult r = signal::HeartRateEstimator::estimate(beats, 10000);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(10, r.beat_count);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 60000.0f / 857.0f, r.hr_bpm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, r.consistency);
}

void test_hr_invalid_when_too_few_beats() {
    bw::RingBuffer<TimeMs, signal::kMaxBeatsInHrWindow> beats;
    beats.push(500);
    beats.push(1357);
    beats.push(2214);
    const signal::HeartRateResult r = signal::HeartRateEstimator::estimate(beats, 10000);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(3, r.beat_count);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.hr_bpm);
}

void test_hr_invalid_when_timing_inconsistent() {
    bw::RingBuffer<TimeMs, signal::kMaxBeatsInHrWindow> beats;
    // Alternating 800/400 ms IBIs: neither lands inside ±20 % of the 800 ms
    // median for the 400s ⇒ consistency 5/9 ≈ 0.556 < 0.6 gate (F02 D6).
    const TimeMs stamps[] = {0, 800, 1200, 2000, 2400, 3200, 3600, 4400, 4800, 5600};
    for (TimeMs t : stamps) {
        beats.push(t);
    }
    const signal::HeartRateResult r = signal::HeartRateEstimator::estimate(beats, 6000);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.consistency < signal::kMinIbiConsistencyFraction);
}

void test_hr_rejects_impossible_median() {
    bw::RingBuffer<TimeMs, signal::kMaxBeatsInHrWindow> beats;
    // A 200 ms median = 300 bpm — impossible; the refractory-derived lower
    // bound must reject it even though the timing is perfectly consistent.
    for (int k = 0; k < 10; ++k) {
        beats.push(static_cast<TimeMs>(k * 200));
    }
    const signal::HeartRateResult r = signal::HeartRateEstimator::estimate(beats, 2000);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, r.consistency);
}

// ---- PpgQualityEstimator ---------------------------------------------------

namespace {

signal::PpgFrameStats cleanStats() {
    signal::PpgFrameStats s;
    s.sample_count = 100;  // one full second at 100 Hz
    s.sum_raw_ir = 100.0f * 50000.0f;
    s.sum_filtered_sq = 100.0f * 707.0f * 707.0f;  // AC rms 707 ⇒ PI 1.4 %
    s.max_gap_ms = signal::kPpgSamplePeriodMs;
    s.finalize(signal::kMotionWindowMs);
    return s;
}

}  // namespace

void test_quality_clean_window_good() {
    const signal::PpgFrameStats s = cleanStats();
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Good), static_cast<int>(q.quality));
    TEST_ASSERT_TRUE(q.sqi > 0.9f);
}

void test_quality_no_samples_insufficient() {
    signal::PpgFrameStats s;
    s.finalize(signal::kMotionWindowMs);
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(q.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, q.sqi);
}

void test_quality_low_coverage_insufficient() {
    signal::PpgFrameStats s = cleanStats();
    s.sample_count = 40;  // 40 % < kMinWindowCoverageFrac
    s.finalize(signal::kMotionWindowMs);
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(q.quality));
}

void test_quality_saturation_insufficient() {
    signal::PpgFrameStats s = cleanStats();
    s.saturated_count = 3;  // 3 % >= kMaxSaturatedFraction
    s.finalize(signal::kMotionWindowMs);
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(q.quality));
}

void test_quality_low_perfusion_insufficient() {
    signal::PpgFrameStats s = cleanStats();
    s.sum_filtered_sq = 100.0f * 20.0f * 20.0f;  // AC rms 20 ⇒ PI 0.04 %
    s.finalize(signal::kMotionWindowMs);
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Insufficient), static_cast<int>(q.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, q.sqi);
}

void test_quality_gap_degraded() {
    signal::PpgFrameStats s = cleanStats();
    s.max_gap_ms = signal::kMaxSampleGapMs + 1;
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Degraded), static_cast<int>(q.quality));
}

void test_quality_anomaly_degraded() {
    signal::PpgFrameStats s = cleanStats();
    s.anomaly = true;
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 1.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Degraded), static_cast<int>(q.quality));
}

void test_quality_poor_timing_degrades_sqi() {
    const signal::PpgFrameStats s = cleanStats();
    // Consistency 0.5 with perfect perfusion: sqi = 1 × 0.5 × 1 = 0.5 →
    // passes the 0.4 floor but below full trust.
    const signal::PpgQualityResult q = signal::PpgQualityEstimator::evaluate(s, 0.5f);
    TEST_ASSERT_EQUAL(static_cast<int>(DataQuality::Good), static_cast<int>(q.quality));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, q.sqi);
}
