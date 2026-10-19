// F07 §9 test_smoother.cpp — the EMA behind both evidence channels (D2/D7).

#include <cmath>
#include <limits>

#include <bw/state/smoother.h>
#include <bw/state/state_config.h>
#include <unity.h>

void test_smoother_first_sample_seeds(void) {
    // D7: seeded on the first valid sample — no warm-up lag from 0.0. A
    // channel fed 0.85 must read 0.85 immediately, not climb to it.
    bw::state::Smoother s{bw::state::kEmaAlpha};
    TEST_ASSERT_FALSE(s.seeded());
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.85f, s.step(0.85f));
    TEST_ASSERT_TRUE(s.seeded());
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.85f, s.value());
}

void test_smoother_alpha_applied_per_step(void) {
    bw::state::Smoother s{0.5f};
    s.step(1.0f);  // seed = 1.0
    // value += alpha * (x - value) = 1.0 + 0.5 * (0.0 - 1.0) = 0.5
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.5f, s.step(0.0f));
    // 0.5 + 0.5 * (0.0 - 0.5) = 0.25
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.25f, s.step(0.0f));
}

void test_smoother_reset_reseeds(void) {
    // D6/D7: reset at gate entry, then the next sample re-seeds — recovery is
    // judged from clean data only.
    bw::state::Smoother s{bw::state::kEmaAlpha};
    s.step(0.9f);
    s.step(0.9f);
    s.reset();
    TEST_ASSERT_FALSE(s.seeded());
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, s.step(0.1f));  // seeds at 0.1, no memory
}

void test_smoother_nan_propagates_why_d13_validates_upstream(void) {
    // Documents WHY D13 validates before smoothing: a NaN reaching the EMA
    // pins the channel at NaN forever (every comparison false — E2 would
    // never de-escalate). The engine must reject such input upstream
    // (covered in test_snapshot.cpp); the smoother itself stays dumb.
    bw::state::Smoother s{bw::state::kEmaAlpha};
    s.step(0.5f);
    const float out = s.step(std::numeric_limits<float>::quiet_NaN());
    TEST_ASSERT_TRUE(std::isnan(out));
}
