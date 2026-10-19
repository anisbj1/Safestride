// Test runner for the bw_sleep / F08 suite (§9).
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_state/test_bw_state/main.cpp.)

#include <unity.h>

// ---- classifier (spec §9, §6 D2 table) ---------------------------------------
void test_classifier_row1_degraded_share_above_max_uncertain(void);
void test_classifier_row1_share_exactly_max_passes(void);
void test_classifier_row1_share_epsilon_above_max_uncertain(void);
void test_classifier_row2_duration_at_napmax_is_main_sleep(void);
void test_classifier_row2_napmax_minus_one_is_not_main_sleep(void);
void test_classifier_row2_napmax_plus_one_main_sleep_day(void);
void test_classifier_row3_mode_nap_with_unsynced_clock(void);
void test_classifier_row4_unsynced_clock_day_is_uncertain(void);
void test_classifier_row5_night_short_of_napmax_known_clock_is_nap(void);
void test_classifier_row5_night_unsynced_clock_is_uncertain(void);
void test_classifier_row6_day_synced_is_rest(void);
void test_classifier_row6_calibration_and_research_synced_are_rest(void);
void test_classifier_duration_beats_mode_planned_nap(void);
void test_classifier_varied_config_moves_boundary(void);

// ---- tracker boundaries (spec §9, §6 D1 table, §8 boundaries) -----------------
void test_tracker_row1_identical_t_ms_is_noop(void);
void test_tracker_row2_idle_to_candidate_records_start(void);
void test_tracker_row3_promotes_at_exact_min(void);
void test_tracker_row3_min_minus_one_stays_candidate(void);
void test_tracker_row4_candidate_gap_plus_one_aborts_silently(void);
void test_tracker_row4_candidate_gap_exact_merges(void);
void test_tracker_row4_observed_wakefulness_is_the_gap(void);
void test_tracker_row5_active_return_continues_same_episode(void);
void test_tracker_row6_gap_plus_one_closes(void);
void test_tracker_row7_notworn_close_carries_reason(void);
void test_tracker_notworn_interruption_shorter_than_gap_is_kept(void);
void test_tracker_two_merges_stay_one_episode(void);
void test_tracker_short_span_emits_nothing(void);
void test_tracker_insufficient_source_counts_as_gap(void);
void test_tracker_row8_backwards_closes_active_uncertain(void);
void test_tracker_row8_backwards_candidate_goes_idle_silently(void);
void test_tracker_first_call_with_awake_is_noop(void);
void test_tracker_w2_nullptr_deps_full_episode(void);

// ---- recovery (spec §9, §6 D4, §10) ------------------------------------------
void test_recovery_marker_present_emits_one_recovered_event(void);
void test_recovery_marker_absent_no_event(void);
void test_recovery_truncated_marker_ignored(void);
void test_recovery_schema_mismatch_ignored(void);
void test_recovery_second_call_is_silent(void);
void test_recovery_second_boot_after_recover_is_silent(void);
void test_recovery_kv_nullptr_no_event_no_crash(void);
void test_recovery_marker_cleared_after_normal_close(void);
void test_recovery_put_count_open_and_close_per_episode(void);
void test_recovery_put_failure_at_promotion_still_active(void);

// ---- events & status (spec §9, §6 D5/D6/D7) ----------------------------------
void test_events_open_marker_fields(void);
void test_events_close_cleared_flags_and_final_class(void);
void test_events_provisional_vs_final_class_bytes(void);
void test_events_t_start_unix_captured_at_candidate_start(void);
void test_events_t_start_unix_zero_when_never_synced(void);
void test_events_share_exactly_max_is_not_uncertain(void);
void test_events_share_epsilon_over_max_is_uncertain(void);
void test_events_confidence_is_mean_times_one_minus_share(void);
void test_status_changed_only_on_phase_or_start_change(void);
void test_status_values_across_phases(void);
void test_events_delivered_through_sink_pair_open_and_close(void);
void test_events_duration_saturates_at_uint32_max(void);
void test_config_registry_literals_pinned(void);
void test_budget_contracts_and_size(void);

// ---- wiring sanity (additive; README §3 native-compile rule) ------------------
void test_wiring_constants_match_spec(void);
void test_wiring_wire_is_safe_with_empty_context(void);

// ---- wiring delivery order (audit M-1; §7 attach-order independence) ----------
void test_wire_event_buffered_until_sink_attaches(void);
void test_wire_event_published_directly_when_sink_attaches_first(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_classifier_row1_degraded_share_above_max_uncertain);
    RUN_TEST(test_classifier_row1_share_exactly_max_passes);
    RUN_TEST(test_classifier_row1_share_epsilon_above_max_uncertain);
    RUN_TEST(test_classifier_row2_duration_at_napmax_is_main_sleep);
    RUN_TEST(test_classifier_row2_napmax_minus_one_is_not_main_sleep);
    RUN_TEST(test_classifier_row2_napmax_plus_one_main_sleep_day);
    RUN_TEST(test_classifier_row3_mode_nap_with_unsynced_clock);
    RUN_TEST(test_classifier_row4_unsynced_clock_day_is_uncertain);
    RUN_TEST(test_classifier_row5_night_short_of_napmax_known_clock_is_nap);
    RUN_TEST(test_classifier_row5_night_unsynced_clock_is_uncertain);
    RUN_TEST(test_classifier_row6_day_synced_is_rest);
    RUN_TEST(test_classifier_row6_calibration_and_research_synced_are_rest);
    RUN_TEST(test_classifier_duration_beats_mode_planned_nap);
    RUN_TEST(test_classifier_varied_config_moves_boundary);

    RUN_TEST(test_tracker_row1_identical_t_ms_is_noop);
    RUN_TEST(test_tracker_row2_idle_to_candidate_records_start);
    RUN_TEST(test_tracker_row3_promotes_at_exact_min);
    RUN_TEST(test_tracker_row3_min_minus_one_stays_candidate);
    RUN_TEST(test_tracker_row4_candidate_gap_plus_one_aborts_silently);
    RUN_TEST(test_tracker_row4_candidate_gap_exact_merges);
    RUN_TEST(test_tracker_row4_observed_wakefulness_is_the_gap);
    RUN_TEST(test_tracker_row5_active_return_continues_same_episode);
    RUN_TEST(test_tracker_row6_gap_plus_one_closes);
    RUN_TEST(test_tracker_row7_notworn_close_carries_reason);
    RUN_TEST(test_tracker_notworn_interruption_shorter_than_gap_is_kept);
    RUN_TEST(test_tracker_two_merges_stay_one_episode);
    RUN_TEST(test_tracker_short_span_emits_nothing);
    RUN_TEST(test_tracker_insufficient_source_counts_as_gap);
    RUN_TEST(test_tracker_row8_backwards_closes_active_uncertain);
    RUN_TEST(test_tracker_row8_backwards_candidate_goes_idle_silently);
    RUN_TEST(test_tracker_first_call_with_awake_is_noop);
    RUN_TEST(test_tracker_w2_nullptr_deps_full_episode);

    RUN_TEST(test_recovery_marker_present_emits_one_recovered_event);
    RUN_TEST(test_recovery_marker_absent_no_event);
    RUN_TEST(test_recovery_truncated_marker_ignored);
    RUN_TEST(test_recovery_schema_mismatch_ignored);
    RUN_TEST(test_recovery_second_call_is_silent);
    RUN_TEST(test_recovery_second_boot_after_recover_is_silent);
    RUN_TEST(test_recovery_kv_nullptr_no_event_no_crash);
    RUN_TEST(test_recovery_marker_cleared_after_normal_close);
    RUN_TEST(test_recovery_put_count_open_and_close_per_episode);
    RUN_TEST(test_recovery_put_failure_at_promotion_still_active);

    RUN_TEST(test_events_open_marker_fields);
    RUN_TEST(test_events_close_cleared_flags_and_final_class);
    RUN_TEST(test_events_provisional_vs_final_class_bytes);
    RUN_TEST(test_events_t_start_unix_captured_at_candidate_start);
    RUN_TEST(test_events_t_start_unix_zero_when_never_synced);
    RUN_TEST(test_events_share_exactly_max_is_not_uncertain);
    RUN_TEST(test_events_share_epsilon_over_max_is_uncertain);
    RUN_TEST(test_events_confidence_is_mean_times_one_minus_share);
    RUN_TEST(test_status_changed_only_on_phase_or_start_change);
    RUN_TEST(test_status_values_across_phases);
    RUN_TEST(test_events_delivered_through_sink_pair_open_and_close);
    RUN_TEST(test_events_duration_saturates_at_uint32_max);
    RUN_TEST(test_config_registry_literals_pinned);
    RUN_TEST(test_budget_contracts_and_size);

    RUN_TEST(test_wiring_constants_match_spec);
    RUN_TEST(test_wiring_wire_is_safe_with_empty_context);
    RUN_TEST(test_wire_event_buffered_until_sink_attaches);
    RUN_TEST(test_wire_event_published_directly_when_sink_attaches_first);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
