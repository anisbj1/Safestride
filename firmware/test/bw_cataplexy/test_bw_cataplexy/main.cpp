// Test runner for the bw_cataplexy / F09 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_activity/test_bw_activity/main.cpp.)

#include <unity.h>

// ---- confounder checker ----------------------------------------------------
void test_confounders_clean_drop_has_none(void);
void test_confounders_fall_by_jerk(void);
void test_confounders_fall_by_impact_enmo(void);
void test_confounders_posture_change(void);
void test_confounders_posture_boundary_is_inclusive(void);
void test_confounders_brief_burst_when_earlier_quiet(void);
void test_confounders_brief_burst_when_earlier_unusable(void);
void test_confounders_no_previous_window_objects_nothing(void);
void test_confounders_flags_combine(void);
void test_confounders_thresholds_are_configurable(void);

// ---- candidate detector ----------------------------------------------------
void test_detector_disabled_when_imu_absent(void);
void test_detector_no_snapshot_evaluates_nothing(void);
void test_detector_first_call_is_empty(void);
void test_detector_clean_drop_raises(void);
void test_detector_sitting_down_is_not_raised(void);
void test_detector_fall_is_not_raised(void);
void test_detector_brief_burst_is_not_raised(void);
void test_detector_hr_corroborates_clean_drop(void);
void test_detector_hr_does_not_raise_a_confounder(void);
void test_detector_absent_ppg_evaluates_imu_only(void);
void test_detector_low_confidence_hr_is_ignored(void);
void test_detector_insufficient_quality_gates(void);
void test_detector_degraded_quality_lowers_confidence(void);
void test_detector_not_worn_resets_history(void);
void test_detector_gap_restarts_history(void);
void test_detector_gap_at_max_keeps_history(void);
void test_detector_backwards_time_restarts_history(void);
void test_detector_nan_input_never_raises(void);
void test_detector_confidence_never_without_reasons(void);
void test_detector_event_span_is_the_drop_window(void);
void test_detector_size_within_budget(void);

// ---- event window requester ------------------------------------------------
void test_window_idle_until_marked(void);
void test_window_not_due_before_post_window(void);
void test_window_due_exactly_at_post_boundary(void);
void test_window_clamps_start_at_boot_boundary(void);
void test_window_dropped_outside_research_mode(void);
void test_window_second_mark_ignored_while_pending(void);
void test_window_reset_clears_pending(void);
void test_window_backwards_clock_is_not_due(void);

// ---- event construction ----------------------------------------------------
void test_event_type_and_schema(void);
void test_event_requests_the_raw_window(void);
void test_event_carries_confounder_flags(void);
void test_event_reserved_payload_is_zero(void);
void test_event_identity_is_left_to_f17(void);
void test_event_confidence_and_reasons_travel_together(void);
void test_event_span_and_context_are_copied(void);
void test_event_stays_within_the_fixed_record(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_confounders_clean_drop_has_none);
    RUN_TEST(test_confounders_fall_by_jerk);
    RUN_TEST(test_confounders_fall_by_impact_enmo);
    RUN_TEST(test_confounders_posture_change);
    RUN_TEST(test_confounders_posture_boundary_is_inclusive);
    RUN_TEST(test_confounders_brief_burst_when_earlier_quiet);
    RUN_TEST(test_confounders_brief_burst_when_earlier_unusable);
    RUN_TEST(test_confounders_no_previous_window_objects_nothing);
    RUN_TEST(test_confounders_flags_combine);
    RUN_TEST(test_confounders_thresholds_are_configurable);

    RUN_TEST(test_detector_disabled_when_imu_absent);
    RUN_TEST(test_detector_no_snapshot_evaluates_nothing);
    RUN_TEST(test_detector_first_call_is_empty);
    RUN_TEST(test_detector_clean_drop_raises);
    RUN_TEST(test_detector_sitting_down_is_not_raised);
    RUN_TEST(test_detector_fall_is_not_raised);
    RUN_TEST(test_detector_brief_burst_is_not_raised);
    RUN_TEST(test_detector_hr_corroborates_clean_drop);
    RUN_TEST(test_detector_hr_does_not_raise_a_confounder);
    RUN_TEST(test_detector_absent_ppg_evaluates_imu_only);
    RUN_TEST(test_detector_low_confidence_hr_is_ignored);
    RUN_TEST(test_detector_insufficient_quality_gates);
    RUN_TEST(test_detector_degraded_quality_lowers_confidence);
    RUN_TEST(test_detector_not_worn_resets_history);
    RUN_TEST(test_detector_gap_restarts_history);
    RUN_TEST(test_detector_gap_at_max_keeps_history);
    RUN_TEST(test_detector_backwards_time_restarts_history);
    RUN_TEST(test_detector_nan_input_never_raises);
    RUN_TEST(test_detector_confidence_never_without_reasons);
    RUN_TEST(test_detector_event_span_is_the_drop_window);
    RUN_TEST(test_detector_size_within_budget);

    RUN_TEST(test_window_idle_until_marked);
    RUN_TEST(test_window_not_due_before_post_window);
    RUN_TEST(test_window_due_exactly_at_post_boundary);
    RUN_TEST(test_window_clamps_start_at_boot_boundary);
    RUN_TEST(test_window_dropped_outside_research_mode);
    RUN_TEST(test_window_second_mark_ignored_while_pending);
    RUN_TEST(test_window_reset_clears_pending);
    RUN_TEST(test_window_backwards_clock_is_not_due);

    RUN_TEST(test_event_type_and_schema);
    RUN_TEST(test_event_requests_the_raw_window);
    RUN_TEST(test_event_carries_confounder_flags);
    RUN_TEST(test_event_reserved_payload_is_zero);
    RUN_TEST(test_event_identity_is_left_to_f17);
    RUN_TEST(test_event_confidence_and_reasons_travel_together);
    RUN_TEST(test_event_span_and_context_are_copied);
    RUN_TEST(test_event_stays_within_the_fixed_record);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
