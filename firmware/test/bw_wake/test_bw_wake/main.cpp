// Test runner for the bw_wake / F06 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_activity/test_bw_activity/main.cpp — one strong
// setUp/tearDown definition per suite, weak defaults do not link on this
// toolchain.)

#include <unity.h>
void test_input_gate_reproduces_phased_window_end_latency(void);
void test_input_gate_motion_age_boundary_preserves_original_evidence_stamp(void);
void test_input_gate_response_age_does_not_inherit_motion_pipeline_budget(void);
void test_input_gate_distinguishes_wear_quality_and_invalid_motion(void);
void test_input_gate_separates_cadence_and_missing_epoch_from_stale_motion(void);
void test_motion_telemetry_integer_conversion_preserves_threshold_and_rejects_bad_values(void);
void test_demo_motion_evidence_is_current_not_held_awake(void);
void test_demo_recent_awake_expires_and_bad_motion_clears_hold(void);
void test_demo_motion_reasons_reach_contract_and_notworn_resets_burst(void);
void test_demo_initial_contact_grace_hands_off_to_background_epoch_without_gap(void);
void test_demo_gentle_motion_wakes_without_arming_strong_burst(void);
void test_demo_gentle_motion_threshold_boundary_and_override(void);

// ---- feature assembler -----------------------------------------------------
void test_assembler_first_call_accumulates(void);
void test_assembler_commits_at_epoch_boundary(void);
void test_assembler_means_and_low_motion_ratio(void);
void test_assembler_insufficient_imu_invalidates_epoch(void);
void test_assembler_missing_ppg_degrades_but_keeps_estimate(void);
void test_assembler_modality_quorum(void);
void test_assembler_baseline_absent_is_neutral(void);
void test_assembler_baseline_z_score(void);
void test_assembler_baseline_mad_floor(void);
void test_assembler_time_of_day(void);
void test_assembler_backwards_timestamp_commits_and_restarts(void);
void test_assembler_zero_epoch_length_never_commits(void);
void test_assembler_history_is_bounded(void);
void test_assembler_dominant_activity(void);
void test_assembler_low_confidence_activity_is_ignored(void);
void test_build_vector_normalization_and_schema_stamp(void);
void test_build_vector_missing_inputs_land_on_identity(void);
void test_build_vector_time_of_day_is_circular(void);
void test_build_vector_rejects_empty_epoch(void);
void test_history_previous_epoch_invariant(void);
void test_low_motion_streak_counts_and_breaks(void);
void test_low_motion_streak_breaks_on_unusable_epoch(void);
void test_enmo_drop_ratio(void);

// ---- heuristic estimator ---------------------------------------------------
void test_heuristic_walking_reads_as_awake(void);
void test_heuristic_sustained_stillness_reads_as_drowsy(void);
void test_heuristic_probabilities_sum_to_one(void);
void test_heuristic_monotonic_in_motion(void);
void test_heuristic_monotonic_in_heart_rate(void);
void test_heuristic_invalid_when_imu_insufficient(void);
void test_heuristic_works_with_baseline_absent(void);
void test_heuristic_low_hr_below_baseline_sets_reason(void);
void test_heuristic_sparse_hr_coverage_gives_no_hr_term(void);
void test_heuristic_degraded_quality_lowers_confidence(void);
void test_heuristic_missing_ppg_flagged_and_discounted(void);
void test_heuristic_reason_threshold_is_half_evidence(void);
void test_heuristic_time_of_day_window(void);
void test_heuristic_sleep_needs_duration_evidence(void);
void test_heuristic_first_epoch_has_no_trend_terms(void);
void test_heuristic_gap_epoch_is_not_a_trend_reference(void);
void test_heuristic_insufficient_quality_is_invalid(void);
void test_heuristic_non_finite_weights_report_no_estimate(void);
void test_config_zero_feature_window_reports_no_expectation(void);

// ---- model estimator -------------------------------------------------------
void test_model_absent_reports_no_estimate(void);
void test_model_predict_maps_classes_and_stamps_schema(void);
void test_model_wrong_class_count_is_rejected(void);
void test_model_predict_error_reports_no_estimate(void);
void test_model_skips_prediction_on_unusable_epoch(void);
void test_model_insufficient_quality_skips_prediction(void);
void test_model_contributions_map_to_reason_codes(void);
void test_model_small_contributions_are_not_quoted(void);
void test_model_unsupported_contributions_keep_quality_reasons(void);
void test_model_confidence_is_capped_by_data_quality(void);

// ---- wakefulness estimator (L2 entry point) --------------------------------
void test_estimator_no_estimate_before_first_epoch(void);
void test_estimator_emits_at_boundary_and_holds_within_epoch(void);
void test_estimator_moving_reads_awake_resting_reads_drowsy(void);
void test_estimator_reset_clears_history_and_answer(void);
void test_estimator_falls_back_to_heuristic_when_model_fails(void);
void test_estimator_uses_model_when_it_answers(void);
void test_estimator_sparse_epoch_reports_no_estimate(void);
void test_estimator_mixed_window_quality_reports_no_estimate(void);
void test_fast_lane_requires_burst_then_emits_each_low_motion_tick(void);
void test_fast_lane_sub_burst_motion_publishes_and_holds_awake(void);
void test_fast_lane_motion_resets_to_awake_and_check_holds_until_burst(void);
void test_fast_lane_stale_or_gapped_imu_is_invalid_and_epoch_cannot_override(void);
void test_wake_sizes_within_budget(void);

// ---- schema ----------------------------------------------------------------
void test_schema_ids_are_never_zero(void);
void test_schema_fits_the_static_bound(void);
void test_schema_names_are_unique_and_snake_case(void);
void test_schema_normalization_is_sane(void);
void test_schema_none_method_is_identity(void);
void test_schema_reason_table_only_holds_real_codes(void);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_input_gate_reproduces_phased_window_end_latency);
    RUN_TEST(test_input_gate_motion_age_boundary_preserves_original_evidence_stamp);
    RUN_TEST(test_input_gate_response_age_does_not_inherit_motion_pipeline_budget);
    RUN_TEST(test_input_gate_distinguishes_wear_quality_and_invalid_motion);
    RUN_TEST(test_input_gate_separates_cadence_and_missing_epoch_from_stale_motion);
    RUN_TEST(test_motion_telemetry_integer_conversion_preserves_threshold_and_rejects_bad_values);
    RUN_TEST(test_demo_motion_evidence_is_current_not_held_awake);
    RUN_TEST(test_demo_recent_awake_expires_and_bad_motion_clears_hold);
    RUN_TEST(test_demo_motion_reasons_reach_contract_and_notworn_resets_burst);
    RUN_TEST(test_demo_initial_contact_grace_hands_off_to_background_epoch_without_gap);
    RUN_TEST(test_demo_gentle_motion_wakes_without_arming_strong_burst);
    RUN_TEST(test_demo_gentle_motion_threshold_boundary_and_override);

    RUN_TEST(test_assembler_first_call_accumulates);
    RUN_TEST(test_assembler_commits_at_epoch_boundary);
    RUN_TEST(test_assembler_means_and_low_motion_ratio);
    RUN_TEST(test_assembler_insufficient_imu_invalidates_epoch);
    RUN_TEST(test_assembler_missing_ppg_degrades_but_keeps_estimate);
    RUN_TEST(test_assembler_modality_quorum);
    RUN_TEST(test_assembler_baseline_absent_is_neutral);
    RUN_TEST(test_assembler_baseline_z_score);
    RUN_TEST(test_assembler_baseline_mad_floor);
    RUN_TEST(test_assembler_time_of_day);
    RUN_TEST(test_assembler_backwards_timestamp_commits_and_restarts);
    RUN_TEST(test_assembler_zero_epoch_length_never_commits);
    RUN_TEST(test_assembler_history_is_bounded);
    RUN_TEST(test_assembler_dominant_activity);
    RUN_TEST(test_assembler_low_confidence_activity_is_ignored);
    RUN_TEST(test_build_vector_normalization_and_schema_stamp);
    RUN_TEST(test_build_vector_missing_inputs_land_on_identity);
    RUN_TEST(test_build_vector_time_of_day_is_circular);
    RUN_TEST(test_build_vector_rejects_empty_epoch);
    RUN_TEST(test_history_previous_epoch_invariant);
    RUN_TEST(test_low_motion_streak_counts_and_breaks);
    RUN_TEST(test_low_motion_streak_breaks_on_unusable_epoch);
    RUN_TEST(test_enmo_drop_ratio);

    RUN_TEST(test_heuristic_walking_reads_as_awake);
    RUN_TEST(test_heuristic_sustained_stillness_reads_as_drowsy);
    RUN_TEST(test_heuristic_probabilities_sum_to_one);
    RUN_TEST(test_heuristic_monotonic_in_motion);
    RUN_TEST(test_heuristic_monotonic_in_heart_rate);
    RUN_TEST(test_heuristic_invalid_when_imu_insufficient);
    RUN_TEST(test_heuristic_works_with_baseline_absent);
    RUN_TEST(test_heuristic_low_hr_below_baseline_sets_reason);
    RUN_TEST(test_heuristic_sparse_hr_coverage_gives_no_hr_term);
    RUN_TEST(test_heuristic_degraded_quality_lowers_confidence);
    RUN_TEST(test_heuristic_missing_ppg_flagged_and_discounted);
    RUN_TEST(test_heuristic_reason_threshold_is_half_evidence);
    RUN_TEST(test_heuristic_time_of_day_window);
    RUN_TEST(test_heuristic_sleep_needs_duration_evidence);
    RUN_TEST(test_heuristic_first_epoch_has_no_trend_terms);
    RUN_TEST(test_heuristic_gap_epoch_is_not_a_trend_reference);
    RUN_TEST(test_heuristic_insufficient_quality_is_invalid);
    RUN_TEST(test_heuristic_non_finite_weights_report_no_estimate);
    RUN_TEST(test_config_zero_feature_window_reports_no_expectation);

    RUN_TEST(test_model_absent_reports_no_estimate);
    RUN_TEST(test_model_predict_maps_classes_and_stamps_schema);
    RUN_TEST(test_model_wrong_class_count_is_rejected);
    RUN_TEST(test_model_predict_error_reports_no_estimate);
    RUN_TEST(test_model_skips_prediction_on_unusable_epoch);
    RUN_TEST(test_model_insufficient_quality_skips_prediction);
    RUN_TEST(test_model_contributions_map_to_reason_codes);
    RUN_TEST(test_model_small_contributions_are_not_quoted);
    RUN_TEST(test_model_unsupported_contributions_keep_quality_reasons);
    RUN_TEST(test_model_confidence_is_capped_by_data_quality);

    RUN_TEST(test_estimator_no_estimate_before_first_epoch);
    RUN_TEST(test_estimator_emits_at_boundary_and_holds_within_epoch);
    RUN_TEST(test_estimator_moving_reads_awake_resting_reads_drowsy);
    RUN_TEST(test_estimator_reset_clears_history_and_answer);
    RUN_TEST(test_estimator_falls_back_to_heuristic_when_model_fails);
    RUN_TEST(test_estimator_uses_model_when_it_answers);
    RUN_TEST(test_estimator_sparse_epoch_reports_no_estimate);
    RUN_TEST(test_estimator_mixed_window_quality_reports_no_estimate);
    RUN_TEST(test_fast_lane_requires_burst_then_emits_each_low_motion_tick);
    RUN_TEST(test_fast_lane_sub_burst_motion_publishes_and_holds_awake);
    RUN_TEST(test_fast_lane_motion_resets_to_awake_and_check_holds_until_burst);
    RUN_TEST(test_fast_lane_stale_or_gapped_imu_is_invalid_and_epoch_cannot_override);
    RUN_TEST(test_wake_sizes_within_budget);

    RUN_TEST(test_schema_ids_are_never_zero);
    RUN_TEST(test_schema_fits_the_static_bound);
    RUN_TEST(test_schema_names_are_unique_and_snake_case);
    RUN_TEST(test_schema_normalization_is_sane);
    RUN_TEST(test_schema_none_method_is_identity);
    RUN_TEST(test_schema_reason_table_only_holds_real_codes);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
