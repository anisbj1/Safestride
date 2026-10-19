// F13 §9 tests 13, 16, 17 — warm restart, the size budgets (§11) and the
// time-free / reboot-boundary properties of the persisted state.

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_serializer.h>
#include <bw/baseline/baseline_tracker.h>

#include "fixtures.h"

namespace bl = bw::baseline;

// §9.13 — serialize a valid state, restore into a new tracker: the profile is
// valid immediately, alpha stays at the steady window (no re-ramp), the
// outlier gate is already armed, and learning continues from the count.
void test_warm_restart_keeps_state(void) {
    bl::BaselineTracker first;
    for (uint32_t i = 0; i < 1800; ++i) {
        TEST_ASSERT_TRUE(first.step(f13_test::goodSample()).accepted);
    }
    bw::BaselineProfile p1{};
    TEST_ASSERT_TRUE(first.profile(p1));
    TEST_ASSERT_TRUE(p1.valid);

    uint8_t blob[64];
    TEST_ASSERT_TRUE(bl::BaselineSerializer::serialize(first.state(), blob, sizeof(blob)).ok());
    bl::TrackerState restored{};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::deserialize(blob, bl::kBlobBytes, restored).ok());

    bl::BaselineTracker second;
    TEST_ASSERT_TRUE(second.restore(restored).ok());
    bw::BaselineProfile p2{};
    TEST_ASSERT_TRUE(second.profile(p2));
    TEST_ASSERT_TRUE(p2.valid);  // valid immediately, no 30 min relearning (§10)
    TEST_ASSERT_EQUAL_FLOAT(p1.resting_hr_bpm, p2.resting_hr_bpm);
    TEST_ASSERT_EQUAL_FLOAT(p1.resting_hr_mad, p2.resting_hr_mad);
    TEST_ASSERT_EQUAL_FLOAT(p1.typical_enmo_g, p2.typical_enmo_g);
    TEST_ASSERT_EQUAL_UINT8(p1.confidence_pct, p2.confidence_pct);

    // Alpha is the steady window, not the ramp: one in-band sample moves the
    // estimate by (x - ewma)/kEwmaSteadyWindowSamples (the ramp alternative
    // would be ~17x larger: /1801 for n = 1800).
    const float before = second.state().hr_ewma;
    const float alpha = 1.0f / static_cast<float>(bl::kEwmaSteadyWindowSamples);
    TEST_ASSERT_TRUE(second.step(f13_test::goodSample(64.0f)).accepted);
    const float expected = before + alpha * (64.0f - before);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, expected, second.state().hr_ewma);

    // The post-ramp outlier gate is armed after a restore — the persisted
    // count is past kEwmaRampSamples, so 120 bpm is refused instead of being
    // absorbed by a fresh seeding ramp.
    const bl::StepResult r = second.step(f13_test::goodSample(120.0f));
    TEST_ASSERT_FALSE(r.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::Outlier), static_cast<int>(r.verdict));

    TEST_ASSERT_EQUAL_UINT32(restored.accepted_samples + 1, second.state().accepted_samples);
}

// §9.16 — the §11 budgets, asserted: tracker (+ its filter member) ≤ 256 B,
// blob == 32 B (< the 200 B flash budget), profile == 20 B (the F00 contract
// pin cross-check).
void test_state_sizes_fit_budget(void) {
    TEST_ASSERT_TRUE(sizeof(bl::BaselineTracker) <= 256);  // tracker + filter + config state
    TEST_ASSERT_TRUE(sizeof(bl::QualifiedSampleFilter) <= 64);
    TEST_ASSERT_EQUAL_UINT32(20, sizeof(bw::BaselineProfile));  // F00 contract pin cross-check
    TEST_ASSERT_EQUAL_UINT32(bl::kBlobBytes, 32u);
    TEST_ASSERT_TRUE(bl::kBlobBytes <= 200);  // §11 flash < 200 B

    const bl::TrackerState s{};
    uint8_t buf[64];
    const bw::Result<size_t> enc = bl::BaselineSerializer::serialize(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(enc.ok());
    TEST_ASSERT_EQUAL_UINT32(bl::kBlobBytes, enc.value);  // what lands in the store is 32 B
}

// §9.17 — v1 state is time-free: step() takes no clock, a uint64-scale wall
// clock gap between two halves of one stream cannot change the result, and a
// serialize/restore "reboot" in the middle yields the same state as an
// uninterrupted run (no TimeMs can wrap or be lost — there is none).
void test_time_gap_and_reboot_boundary(void) {
    // The tracker state has exactly 3 floats + a u32: no TimeMs member exists
    // to overflow, wrap or desync across a boot.
    static_assert(sizeof(bl::TrackerState) == 3 * sizeof(float) + sizeof(uint32_t),
                  "persisted state must stay time-free (§9 test 17)");

    auto sample_at = [](int i) {
        // In-band, resting, qualified — the index stands for "tick i", not time.
        const float hr = (i % 3 == 0) ? 58.0f : ((i % 3 == 1) ? 60.0f : 62.0f);
        return f13_test::goodSample(hr);
    };

    // Run A: one continuous stream of 100 ticks.
    bl::BaselineTracker a;
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT_TRUE(a.step(sample_at(i)).accepted);
    }

    // Run B: the same 100 ticks split by a reboot — the first 40 run, their
    // state is serialized ("reboot"), then a fresh tracker restores it and
    // runs the remaining 60. Since the learner state holds no time, neither
    // the split nor the (conceptually huge) gap can be observed.
    bl::BaselineTracker b;
    for (int i = 0; i < 40; ++i) {
        TEST_ASSERT_TRUE(b.step(sample_at(i)).accepted);
    }
    uint8_t blob[64];
    TEST_ASSERT_TRUE(bl::BaselineSerializer::serialize(b.state(), blob, sizeof(blob)).ok());
    bl::BaselineTracker c;
    bl::TrackerState restored{};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::deserialize(blob, bl::kBlobBytes, restored).ok());
    TEST_ASSERT_TRUE(c.restore(restored).ok());
    for (int i = 40; i < 100; ++i) {
        TEST_ASSERT_TRUE(b.step(sample_at(i)).accepted);
        TEST_ASSERT_TRUE(c.step(sample_at(i)).accepted);
    }
    TEST_ASSERT_TRUE(f13_test::sameState(a.state(), b.state()));  // gap invisible
    TEST_ASSERT_TRUE(f13_test::sameState(a.state(), c.state()));  // reboot invisible
}
