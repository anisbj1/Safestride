// F13 §9 tests 4, 5, 6, 7, 9 — the gate layer: outlier band (with its exact
// boundaries), not-worn exclusion, quality exclusion, activity gate with the
// D2 calibration widening, and the HR plausibility clamp.

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_tracker.h>

#include <limits>

#include "fixtures.h"

namespace bl = bw::baseline;

void test_nonfinite_motion_cannot_poison_baseline(void) {
    bl::BaselineTracker tracker;
    const float bad[] = {std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()};
    for (float value : bad) {
        auto sample = f13_test::goodSample();
        sample.enmo_g = value;
        TEST_ASSERT_FALSE(tracker.step(sample).accepted);
        TEST_ASSERT_EQUAL_UINT32(0, tracker.state().accepted_samples);
    }
    TEST_ASSERT_TRUE(tracker.step(f13_test::goodSample()).accepted);
    TEST_ASSERT_EQUAL_FLOAT(0.05f, tracker.state().enmo_ewma);
}

namespace {

// A post-ramp seeded state so the gate under test is the steady branch:
// band = max(kMinOutlierBandBpm, kOutlierBandSigma * mad).
bl::BaselineTracker seededTracker(float hr, float mad) {
    bl::BaselineTracker t;
    bl::TrackerState s{};
    s.hr_ewma = hr;
    s.hr_absdev_ewma = mad;
    s.enmo_ewma = 0.05f;
    s.accepted_samples = 10000;  // far past kEwmaRampSamples — no re-seeding
    TEST_ASSERT_TRUE(t.restore(s).ok());
    return t;
}

int verdict(const bl::StepResult& r) {
    return static_cast<int>(r.verdict);
}

}  // namespace

// §9.4 — spikes are rejected with an epsilon-sized shift, and the two boundary
// rows (exactly at max(floor, sigma*mad)) are deterministic, no flapping.
void test_outlier_rejection_and_band_floor(void) {
    // --- boundary at 4.0 * 2.5 = 10 bpm (sigma branch of the band) ---
    bl::BaselineTracker t = seededTracker(60.0f, 2.5f);
    bl::StepResult r = t.step(f13_test::goodSample(70.0f));  // |70-60| == band: accepted
    TEST_ASSERT_TRUE(r.accepted);
    r = t.step(f13_test::goodSample(70.1f));  // just past the band
    TEST_ASSERT_FALSE(r.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::Outlier), verdict(r));

    // --- boundary at the kMinOutlierBandBpm floor (mad small → band = 8) ---
    bl::BaselineTracker tf = seededTracker(60.0f, 0.1f);
    TEST_ASSERT_TRUE(tf.step(f13_test::goodSample(68.0f)).accepted);  // |68-60| == 8: accepted
    r = tf.step(f13_test::goodSample(68.01f));                        // just past the floor
    TEST_ASSERT_FALSE(r.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::Outlier), verdict(r));
    // and the pattern repeats identically — boundary values never flap:
    TEST_ASSERT_TRUE(tf.step(f13_test::goodSample(68.0f)).accepted);
    r = tf.step(f13_test::goodSample(68.01f));
    TEST_ASSERT_FALSE(r.accepted);

    // --- steady stream + single 120 / 30 bpm spikes: shift < epsilon ---
    bl::BaselineTracker t2;
    for (int i = 0; i < 700; ++i) {  // > kEwmaRampSamples: the gate is live
        TEST_ASSERT_TRUE(t2.step(f13_test::goodSample()).accepted);
    }
    const bl::TrackerState before = t2.state();
    r = t2.step(f13_test::goodSample(120.0f));  // far above band (floor 8 here)
    TEST_ASSERT_FALSE(r.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::Outlier), verdict(r));
    r = t2.step(f13_test::goodSample(30.0f));
    TEST_ASSERT_FALSE(r.accepted);
    TEST_ASSERT_TRUE(f13_test::sameState(before, t2.state()));  // refused ⇒ bit-identical
    TEST_ASSERT_TRUE(t2.step(f13_test::goodSample()).accepted);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 60.0f, t2.state().hr_ewma);  // spikes shifted nothing
}

// §9.5 — Worn → NotWorn → Worn: the not-worn stretch leaves the estimates
// bit-identical (§5 rule 1: nothing is learned off-wrist).
void test_not_worn_windows_excluded(void) {
    bl::BaselineTracker t;
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
    }
    const bl::TrackerState before = t.state();

    bw::baseline::BaselineSample off = f13_test::goodSample();
    off.wear = bw::WearState::NotWorn;
    for (int i = 0; i < 50; ++i) {
        const bl::StepResult r = t.step(off);
        TEST_ASSERT_FALSE(r.accepted);
        TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::NotWorn), verdict(r));
    }
    TEST_ASSERT_TRUE(f13_test::sameState(before, t.state()));

    TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);  // Worn again ⇒ learning resumes
    TEST_ASSERT_EQUAL_UINT32(before.accepted_samples + 1, t.state().accepted_samples);
}

// §9.6 — every bad-quality flavor refuses the sample (§5 rule 1) and the
// tracker keeps its state; one good sample afterwards proves it still runs.
void test_poor_quality_windows_excluded(void) {
    bl::BaselineTracker t;
    const bl::TrackerState fresh = t.state();

    bw::baseline::BaselineSample s = f13_test::goodSample();
    s.ppg_quality = bw::DataQuality::Degraded;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.ppg_quality = bw::DataQuality::Insufficient;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.motion_quality = bw::DataQuality::Insufficient;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.hr_valid = false;  // PpgFeatures::hr_valid is part of quality_good (§7)
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.motion_corrupted = true;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    // The StateSnapshot SourceQuality half of quality_good (§7 formula).
    s = f13_test::goodSample();
    s.snapshot_ppg = bw::DataQuality::Degraded;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.snapshot_imu = bw::DataQuality::Insufficient;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadQuality), verdict(t.step(s)));

    TEST_ASSERT_TRUE(f13_test::sameState(fresh, t.state()));  // nothing was learned
    TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
}

// §9.7 — Walking/Running never qualify; Standing/LyingResting qualify only
// with calibration=true (D2); mode absent (calibration=false, the pump's D2
// default) behaves exactly like normal mode.
void test_activity_gate_and_calibration_widening(void) {
    bl::BaselineTracker t;

    bw::baseline::BaselineSample s = f13_test::goodSample();
    s.activity = bw::ActivityContext::Walking;
    TEST_ASSERT_FALSE(t.step(s).accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadActivity), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::Running;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadActivity), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::Stationary;
    TEST_ASSERT_TRUE(t.step(s).accepted);  // in both gate variants

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::Standing;  // calibration absent ⇒ normal gate
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadActivity), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::Standing;
    s.calibration = true;  // D2 widening accepts the still posture
    TEST_ASSERT_TRUE(t.step(s).accepted);

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::LyingResting;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadActivity), verdict(t.step(s)));

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::LyingResting;
    s.calibration = true;
    TEST_ASSERT_TRUE(t.step(s).accepted);

    s = f13_test::goodSample();
    s.activity = bw::ActivityContext::InactiveProlonged;  // never resting evidence
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::BadActivity), verdict(t.step(s)));
}

// §9.9 — the plausibility clamp: 200/20 bpm are artifacts, 35/130 are the
// inclusive boundary, and a NaN hr can never slip through the comparisons.
void test_hr_plausibility_clamp(void) {
    bl::BaselineTracker t;
    const bl::TrackerState fresh = t.state();

    bw::baseline::BaselineSample s = f13_test::goodSample(200.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HrOutOfRange), verdict(t.step(s)));
    s = f13_test::goodSample(20.0f);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HrOutOfRange), verdict(t.step(s)));
    s = f13_test::goodSample(34.9f);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HrOutOfRange), verdict(t.step(s)));
    s = f13_test::goodSample(130.1f);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HrOutOfRange), verdict(t.step(s)));

    s = f13_test::goodSample(35.0f);  // boundary inclusive
    TEST_ASSERT_TRUE(t.step(s).accepted);
    s = f13_test::goodSample(130.0f);
    TEST_ASSERT_TRUE(t.step(s).accepted);

    s = f13_test::goodSample(std::numeric_limits<float>::quiet_NaN());
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HrOutOfRange), verdict(t.step(s)));

    TEST_ASSERT_EQUAL_UINT32(fresh.accepted_samples + 2, t.state().accepted_samples);
}
