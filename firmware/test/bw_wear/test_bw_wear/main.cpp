// Test runner for the bw_wear / F04 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_signal/test_bw_signal/main.cpp — one strong
// setUp/tearDown definition per suite, weak defaults do not link on this
// toolchain.)

#include <unity.h>
void test_ambient_noise_never_supports_wear_and_startup_becomes_notworn();

// ---- evidence (spec §7 WearEvidence, D3/D4) -------------------------------
void test_evidence_fresh_sources_scored(void);
void test_evidence_null_sources_absent(void);
void test_evidence_stale_feature_absent(void);
void test_evidence_future_stamp_absent(void);
void test_evidence_zero_dc_ppg_absent(void);
void test_evidence_insufficient_rules(void);
void test_evidence_insufficient_gates_hr_and_sqi_but_keeps_contact(void);
void test_evidence_score_boundaries(void);
void test_evidence_desk_long_still_zero(void);
void test_evidence_nonfinite_inputs_safe(void);

// ---- detector (spec §6 D1/D2, §9, §10) -----------------------------------
void test_detector_defaults_to_uncertain(void);
void test_detector_both_sources_unavailable_immediate_uncertain(void);
void test_detector_enters_worn_after_confirm_dwell(void);
void test_detector_custom_config_shorter_dwell(void);
void test_detector_custom_config_stricter_enter(void);
void test_detector_holds_worn_through_ppg_dropout(void);
void test_detector_not_worn_only_after_removal_dwell(void);
void test_detector_hysteresis_band_holds_worn(void);
void test_detector_oscillating_evidence_resets_dwell(void);
void test_detector_imu_only_worn_with_capped_confidence(void);
void test_detector_imu_only_not_worn_confidence_capped(void);
void test_detector_stale_features_fall_back_to_uncertain(void);
void test_detector_backward_time_restarts_dwell(void);
void test_detector_never_worn_without_positive_evidence(void);
void test_detector_estimate_echoes_tick_time(void);
void test_detector_size_within_budget(void);

// ---- scenarios (spec §9, §14 DoD table) ----------------------------------
void test_scenario_attached_at_rest(void);
void test_scenario_moving(void);
void test_scenario_resting_on_surface(void);
void test_scenario_no_contact_noisy_hr_stays_not_worn(void);
void test_scenario_removed(void);
void test_scenario_loose_contact(void);
void test_scenario_in_pocket(void);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ambient_noise_never_supports_wear_and_startup_becomes_notworn);

    RUN_TEST(test_evidence_fresh_sources_scored);
    RUN_TEST(test_evidence_null_sources_absent);
    RUN_TEST(test_evidence_stale_feature_absent);
    RUN_TEST(test_evidence_future_stamp_absent);
    RUN_TEST(test_evidence_zero_dc_ppg_absent);
    RUN_TEST(test_evidence_insufficient_rules);
    RUN_TEST(test_evidence_insufficient_gates_hr_and_sqi_but_keeps_contact);
    RUN_TEST(test_evidence_score_boundaries);
    RUN_TEST(test_evidence_desk_long_still_zero);
    RUN_TEST(test_evidence_nonfinite_inputs_safe);

    RUN_TEST(test_detector_defaults_to_uncertain);
    RUN_TEST(test_detector_both_sources_unavailable_immediate_uncertain);
    RUN_TEST(test_detector_enters_worn_after_confirm_dwell);
    RUN_TEST(test_detector_custom_config_shorter_dwell);
    RUN_TEST(test_detector_custom_config_stricter_enter);
    RUN_TEST(test_detector_holds_worn_through_ppg_dropout);
    RUN_TEST(test_detector_not_worn_only_after_removal_dwell);
    RUN_TEST(test_detector_hysteresis_band_holds_worn);
    RUN_TEST(test_detector_oscillating_evidence_resets_dwell);
    RUN_TEST(test_detector_imu_only_worn_with_capped_confidence);
    RUN_TEST(test_detector_imu_only_not_worn_confidence_capped);
    RUN_TEST(test_detector_stale_features_fall_back_to_uncertain);
    RUN_TEST(test_detector_backward_time_restarts_dwell);
    RUN_TEST(test_detector_never_worn_without_positive_evidence);
    RUN_TEST(test_detector_estimate_echoes_tick_time);
    RUN_TEST(test_detector_size_within_budget);

    RUN_TEST(test_scenario_attached_at_rest);
    RUN_TEST(test_scenario_moving);
    RUN_TEST(test_scenario_resting_on_surface);
    RUN_TEST(test_scenario_no_contact_noisy_hr_stays_not_worn);
    RUN_TEST(test_scenario_removed);
    RUN_TEST(test_scenario_loose_contact);
    RUN_TEST(test_scenario_in_pocket);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
