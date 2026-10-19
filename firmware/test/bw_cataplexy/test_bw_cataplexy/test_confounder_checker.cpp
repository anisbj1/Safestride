// firmware/test/bw_cataplexy/test_bw_cataplexy/test_confounder_checker.cpp —
// F09 §9: what makes a drop look less like a loss of tone.

#include <unity.h>

#include <bw/cataplexy/confounder_checker.h>

#include "fixtures.h"

namespace {

using bw::cataplexy::CataplexyConfig;
using bw::cataplexy::checkConfounders;
using bw::cataplexy::Confounder;
using bw::cataplexy::confounderBit;
using bw::cataplexy::ConfounderInput;
using bw::cataplexy::ConfounderMask;
using bw::cataplexy::MotionSample;
using bw::cataplexy::testing::hasConfounder;

// A usable, unremarkable drop: active then still, no jerk, no rotation.
ConfounderInput cleanInput() {
    ConfounderInput input;
    input.earlier.valid = true;
    input.earlier.enmo_g = 0.26f;
    input.previous.valid = true;
    input.previous.enmo_g = 0.24f;
    input.previous.jerk_rms_g_per_s = 0.6f;
    input.previous.tilt_deg = 30.0f;
    input.current.valid = true;
    input.current.enmo_g = 0.01f;
    input.current.tilt_deg = 31.0f;
    input.has_previous = true;
    input.has_earlier = true;
    return input;
}

}  // namespace

void test_confounders_clean_drop_has_none(void) {
    const ConfounderInput input = cleanInput();
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_EQUAL_UINT8(0, mask);
}

void test_confounders_fall_by_jerk(void) {
    ConfounderInput input = cleanInput();
    input.previous.jerk_rms_g_per_s = 4.0f;
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::FallLike));
    TEST_ASSERT_FALSE(hasConfounder(mask, Confounder::PostureChange));
    TEST_ASSERT_FALSE(hasConfounder(mask, Confounder::BriefBurst));
}

void test_confounders_fall_by_impact_enmo(void) {
    ConfounderInput input = cleanInput();
    input.previous.enmo_g = 0.9f;
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::FallLike));
}

void test_confounders_posture_change(void) {
    ConfounderInput input = cleanInput();
    input.current.tilt_deg = 75.0f;  // 45° of rotation across the drop
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::PostureChange));
    TEST_ASSERT_FALSE(hasConfounder(mask, Confounder::FallLike));
}

void test_confounders_posture_boundary_is_inclusive(void) {
    ConfounderInput input = cleanInput();
    input.current.tilt_deg = input.previous.tilt_deg + 30.0f;  // exactly the threshold
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::PostureChange));
}

void test_confounders_brief_burst_when_earlier_quiet(void) {
    ConfounderInput input = cleanInput();
    input.earlier.enmo_g = 0.02f;  // below kActiveMotionEnmoG: activity was short
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::BriefBurst));
}

void test_confounders_brief_burst_when_earlier_unusable(void) {
    ConfounderInput input = cleanInput();
    input.earlier.valid = false;
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_TRUE(hasConfounder(mask, Confounder::BriefBurst));
}

void test_confounders_no_previous_window_objects_nothing(void) {
    ConfounderInput input = cleanInput();
    input.has_previous = false;
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_EQUAL_UINT8(0, mask);
}

void test_confounders_flags_combine(void) {
    ConfounderInput input = cleanInput();
    input.previous.jerk_rms_g_per_s = 4.0f;
    input.current.tilt_deg = 75.0f;
    input.earlier.enmo_g = 0.0f;
    const ConfounderMask mask = checkConfounders(input, CataplexyConfig{});
    TEST_ASSERT_EQUAL_UINT8(confounderBit(Confounder::FallLike) |
                                confounderBit(Confounder::PostureChange) |
                                confounderBit(Confounder::BriefBurst),
                            mask);
}

void test_confounders_thresholds_are_configurable(void) {
    ConfounderInput input = cleanInput();
    input.previous.jerk_rms_g_per_s = 4.0f;
    CataplexyConfig config;
    config.fall_jerk_g_per_s = 10.0f;  // F29 tunes the threshold, not the rule
    const ConfounderMask mask = checkConfounders(input, config);
    TEST_ASSERT_FALSE(hasConfounder(mask, Confounder::FallLike));
}
