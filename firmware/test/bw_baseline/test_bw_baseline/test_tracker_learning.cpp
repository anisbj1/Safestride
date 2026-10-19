// F13 §9 tests 1, 2, 3, 14 — profile absence, convergence, drift tracking and
// the confidence ramp of the pure BaselineTracker.

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_tracker.h>

#include "fixtures.h"

namespace bl = bw::baseline;

// §9.1 — first call: no data, no fabricated values.
void test_profile_absent_before_any_data(void) {
    bl::BaselineTracker t;
    bw::BaselineProfile p{};
    p.valid = true;  // poison the output: profile() must not touch it
    p.resting_hr_bpm = -1.0f;
    TEST_ASSERT_FALSE(t.profile(p));
    TEST_ASSERT_EQUAL_UINT32(0, t.state().accepted_samples);
}

// §9.2 — 1800 scripted 1 Hz sitting/worn/good windows at 60 bpm / 0.05 g:
// valid flips exactly at kMinQualifiedMs, estimates inside the plan tolerances.
void test_learning_converges_on_qualified_stream(void) {
    bl::BaselineTracker t;
    const uint32_t needed = bl::kMinQualifiedMs / bl::kMsPerSecond;  // 1800 samples

    for (uint32_t i = 1; i < needed; ++i) {
        const bl::StepResult r = t.step(f13_test::goodSample());
        TEST_ASSERT_TRUE(r.accepted);
        TEST_ASSERT_FALSE(r.valid_transition);
    }
    bw::BaselineProfile p{};
    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_FALSE(p.valid);  // one sample short of kMinQualifiedMs

    const bl::StepResult last = t.step(f13_test::goodSample());
    TEST_ASSERT_TRUE(last.accepted);
    TEST_ASSERT_TRUE(last.valid_transition);  // the D3 extra-persist trigger

    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, p.resting_hr_bpm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.05f, p.typical_enmo_g);
    TEST_ASSERT_EQUAL_UINT32(bl::kProfileVersion, p.version);
}

// §9.3 — level shift 60 → 72: the estimate approaches monotonically, without
// overshoot, and reaches the one-time-constant tolerance after
// kEwmaSteadyWindowSamples more samples.
void test_adaptation_tracks_drift(void) {
    bl::BaselineTracker t;

    // Phase A "60 bpm steady": deterministic 56/64 jitter is a mean-60 resting
    // spread with |dev| = 4, so the robust band sits at 4*4 = 16 bpm. A
    // zero-noise stream would collapse absdev to 0 and pin the band at the
    // 8 bpm floor, which would reject a genuine +12 step forever — the gate is
    // a MAD gate, it needs dispersion to let real drift in (D1 design intent).
    for (int i = 0; i < 1200; ++i) {
        const float hr = (i % 2 == 0) ? 56.0f : 64.0f;
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample(hr)).accepted);
    }
    const float start = t.state().hr_ewma;
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, start);

    // Phase B — step to 72, constant afterwards, steady alpha.
    float prev = start;
    for (uint32_t i = 0; i < bl::kEwmaSteadyWindowSamples; ++i) {
        const bl::StepResult r = t.step(f13_test::goodSample(72.0f));
        TEST_ASSERT_TRUE(r.accepted);  // adapts instead of locking out
        const float now = t.state().hr_ewma;
        TEST_ASSERT_TRUE(now >= prev);   // monotone approach
        TEST_ASSERT_TRUE(now <= 72.0f);  // no overshoot
        prev = now;
    }
    // After one time constant the EWMA closes 1 - 1/e of the ~12 bpm gap:
    // 12/e ≈ 4.4 bpm left — the plan's tolerance band.
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 72.0f, prev);
}

// §9.4 (ramp validity, with test 14) — confidence at kMinQualifiedMs, at
// kFullConfidenceSamples and beyond; never > 100; valid ⇒ at least the
// ramp value the validity boundary carries.
void test_confidence_ramp(void) {
    bl::BaselineTracker t;
    const uint32_t min_s = bl::kMinQualifiedMs / bl::kMsPerSecond;       // 1800
    const uint32_t full = bl::kFullConfidenceSamples;                    // 7200
    const uint32_t pct_at_valid = min_s * bl::kConfidencePctMax / full;  // 1800 * 100 / 7200 = 25

    for (uint32_t i = 1; i < min_s; ++i) {
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
    }
    bw::BaselineProfile p{};
    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_FALSE(p.valid);
    TEST_ASSERT_EQUAL_UINT32((min_s - 1) * bl::kConfidencePctMax / full, p.confidence_pct);
    TEST_ASSERT_TRUE(p.confidence_pct <= bl::kConfidencePctMax);

    TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);  // min_s-th sample
    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_EQUAL_UINT32(pct_at_valid, p.confidence_pct);
    TEST_ASSERT_TRUE(p.confidence_pct >= pct_at_valid);  // valid ⇒ not below the boundary ramp

    while (t.state().accepted_samples < full) {
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
    }
    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_EQUAL_UINT32(bl::kConfidencePctMax, p.confidence_pct);

    for (int i = 0; i < 500; ++i) {  // far past full evidence: still 100, never more
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
    }
    TEST_ASSERT_TRUE(t.profile(p));
    TEST_ASSERT_EQUAL_UINT32(bl::kConfidencePctMax, p.confidence_pct);
    TEST_ASSERT_TRUE(p.confidence_pct <= bl::kConfidencePctMax);
}
