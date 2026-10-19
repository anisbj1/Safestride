// Test runner for the bw_activity / F03 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_signal/test_bw_signal/main.cpp — one strong
// setUp/tearDown definition per suite, weak defaults do not link on this
// toolchain.)

#include <unity.h>

// ---- cadence detector ------------------------------------------------------
void test_cadence_none_at_zero_frequency(void);
void test_cadence_none_on_nan_frequency(void);
void test_cadence_band_boundaries(void);
void test_cadence_walk_rejects_low_energy(void);
void test_cadence_overlap_low_energy_is_walking(void);
void test_cadence_overlap_high_energy_is_running(void);
void test_cadence_walk_hysteresis_holds_below_min(void);
void test_cadence_walk_hysteresis_holds_above_max(void);
void test_cadence_run_falls_back_to_walk(void);
void test_cadence_run_exits_when_energy_drops(void);
void test_cadence_energy_exit_from_walk(void);
void test_cadence_overlap_hysteresis_holds_walking(void);
void test_cadence_overlap_alternation_does_not_toggle(void);
void test_cadence_run_hysteresis_holds_above_max(void);
void test_cadence_none_on_non_finite_enmo(void);

// ---- classifier ------------------------------------------------------------
void test_classifier_decision_table(void);
void test_classifier_first_call_is_unknown(void);
void test_classifier_dwell_needs_three_windows(void);
void test_classifier_dwell_blip_does_not_flap(void);
void test_classifier_dwell_gap_resets(void);
void test_classifier_backwards_timestamp_resets_dwell(void);
void test_classifier_insufficient_commits_unknown_immediately(void);
void test_classifier_prolonged_inactivity_boundary(void);
void test_classifier_posture_never_sitting_or_standing(void);
void test_classifier_floor_suppresses_low_confidence(void);
void test_classifier_size_within_budget(void);
void test_classifier_candidate_confidence_refreshed_during_dwell(void);
void test_classifier_step_gap_boundary(void);
void test_classifier_tilt_band_inclusivity(void);
void test_classifier_insufficient_beats_duration_and_cadence(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_cadence_none_at_zero_frequency);
    RUN_TEST(test_cadence_none_on_nan_frequency);
    RUN_TEST(test_cadence_band_boundaries);
    RUN_TEST(test_cadence_walk_rejects_low_energy);
    RUN_TEST(test_cadence_overlap_low_energy_is_walking);
    RUN_TEST(test_cadence_overlap_high_energy_is_running);
    RUN_TEST(test_cadence_walk_hysteresis_holds_below_min);
    RUN_TEST(test_cadence_walk_hysteresis_holds_above_max);
    RUN_TEST(test_cadence_run_falls_back_to_walk);
    RUN_TEST(test_cadence_run_exits_when_energy_drops);
    RUN_TEST(test_cadence_energy_exit_from_walk);
    RUN_TEST(test_cadence_overlap_hysteresis_holds_walking);
    RUN_TEST(test_cadence_overlap_alternation_does_not_toggle);
    RUN_TEST(test_cadence_run_hysteresis_holds_above_max);
    RUN_TEST(test_cadence_none_on_non_finite_enmo);

    RUN_TEST(test_classifier_decision_table);
    RUN_TEST(test_classifier_first_call_is_unknown);
    RUN_TEST(test_classifier_dwell_needs_three_windows);
    RUN_TEST(test_classifier_dwell_blip_does_not_flap);
    RUN_TEST(test_classifier_dwell_gap_resets);
    RUN_TEST(test_classifier_backwards_timestamp_resets_dwell);
    RUN_TEST(test_classifier_insufficient_commits_unknown_immediately);
    RUN_TEST(test_classifier_prolonged_inactivity_boundary);
    RUN_TEST(test_classifier_posture_never_sitting_or_standing);
    RUN_TEST(test_classifier_floor_suppresses_low_confidence);
    RUN_TEST(test_classifier_size_within_budget);
    RUN_TEST(test_classifier_candidate_confidence_refreshed_during_dwell);
    RUN_TEST(test_classifier_step_gap_boundary);
    RUN_TEST(test_classifier_tilt_band_inclusivity);
    RUN_TEST(test_classifier_insufficient_beats_duration_and_cadence);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
