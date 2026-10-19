// F13 §9 test 8 — the D5 poisoning guard: held samples are ignored, the
// kPostEventHoldSamples cooldown drains after the hold clears, and learning
// resumes exactly at the boundary (§5 rule 2).

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_tracker.h>

#include "fixtures.h"

namespace bl = bw::baseline;

namespace {

int verdict(const bl::StepResult& r) {
    return static_cast<int>(r.verdict);
}

}  // namespace

void test_poisoning_hold_and_cooldown(void) {
    bl::BaselineTracker t;
    for (int i = 0; i < 50; ++i) {
        TEST_ASSERT_TRUE(t.step(f13_test::goodSample()).accepted);
    }
    const bl::TrackerState learned = t.state();

    // hold=true refuses otherwise-perfect samples and arms the guard.
    bw::baseline::BaselineSample held = f13_test::goodSample();
    held.hold = true;
    for (int i = 0; i < 5; ++i) {
        const bl::StepResult r = t.step(held);
        TEST_ASSERT_FALSE(r.accepted);
        TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HoldActive), verdict(r));
    }
    TEST_ASSERT_TRUE(f13_test::sameState(learned, t.state()));

    // Poisoning dominates verdict precedence: hold + NotWorn still reports
    // HoldActive (the guard, not the wear gate, is why learning stopped).
    held.wear = bw::WearState::NotWorn;
    TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HoldActive), verdict(t.step(held)));
    held = f13_test::goodSample();  // reset the helper copy

    // Hold clears: the cooldown still refuses exactly
    // kPostEventHoldSamples qualified samples (10 min at 1 Hz).
    for (uint32_t i = 0; i < bl::kPostEventHoldSamples; ++i) {
        const bl::StepResult r = t.step(held);  // held is now a good sample again
        TEST_ASSERT_FALSE(r.accepted);
        TEST_ASSERT_EQUAL(static_cast<int>(bl::SampleVerdict::HoldActive), verdict(r));
    }
    TEST_ASSERT_TRUE(
        f13_test::sameState(learned, t.state()));  // nothing learned through either phase

    // ...and the very next sample is accepted: learning resumes at the boundary.
    const bl::StepResult resumed = t.step(f13_test::goodSample());
    TEST_ASSERT_TRUE(resumed.accepted);
    TEST_ASSERT_EQUAL_UINT32(learned.accepted_samples + 1, t.state().accepted_samples);
}
