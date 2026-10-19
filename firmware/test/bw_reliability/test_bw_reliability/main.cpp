// Test runner for the bw_reliability / F26 suite (spec §16.8).
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_diag/test_bw_diag/main.cpp.)
//
// ORDER: pure logic first, then the wiring group. The wiring group shares
// process-wide statics, but every test in it starts with
// reliability_test::resetWiring() (fresh wire() + detached seams), so the
// order inside the group is not load-bearing.

#include <unity.h>

// ---- fault registry (spec §16.8 item 1) --------------------------------------
void test_registry_first_report_fills_empty_state(void);
void test_registry_counts_first_last_and_max_severity(void);
void test_registry_sticky_bits_follow_bit_n_code_n(void);
void test_registry_none_and_out_of_range_are_noops(void);
void test_registry_cr033_codes_are_in_range(void);
void test_registry_bits_are_sticky(void);
void test_registry_interleaved_reports_stay_per_code(void);
void test_registry_works_through_iface_pointer(void);

// ---- watchdog supervisor (spec §16.8 item 2) ---------------------------------
void test_supervisor_all_fresh_feeds(void);
void test_supervisor_tier_a_stall_withholds_feed(void);
void test_supervisor_tier_b_stall_flags_but_keeps_feeding(void);
void test_supervisor_grace_window_feeds_pending_tasks(void);
void test_supervisor_pending_after_grace_does_not_feed(void);
void test_supervisor_nothing_configured_never_feeds(void);
void test_supervisor_newly_stalled_fires_once_per_episode(void);
void test_supervisor_clock_jumps_are_safe(void);
void test_supervisor_configure_is_argument_safe(void);
void test_supervisor_rejects_sentinel_and_invalid_task_ids(void);

// ---- boot guard (spec §16.8 item 3) ------------------------------------------
void test_boot_two_fast_boots_normal_third_safe_mode(void);
void test_boot_survival_clears_streak_once_safe_mode_stays(void);
void test_boot_streak_saturates(void);
void test_boot_reset_reason_maps_to_row8_codes(void);
void test_boot_backwards_clock_never_clears(void);

// ---- reinit scheduler (spec §16.8 item 5) ------------------------------------
void test_reinit_first_attempt_is_due_immediately(void);
void test_reinit_backoff_doubles_to_cap(void);
void test_reinit_not_due_before_horizon(void);
void test_reinit_ok_cancels_and_resets_backoff(void);

// ---- crash record codec (spec §16.8 item 8) ----------------------------------
void test_crash_record_round_trip(void);
void test_crash_record_encode_small_buffer_writes_nothing(void);
void test_crash_record_schema_mismatch_is_absent(void);
void test_crash_record_truncated_or_corrupt_is_absent(void);

// ---- degradation policy, one case per §16.4 row (spec §16.8 item 4) ----------
void test_policy_row1_ppg_failed_sets_flag_with_edge(void);
void test_policy_row1_recovery_clears_flag_despite_sticky_bits(void);
void test_policy_row1_absent_status_uses_latched_codes(void);
void test_policy_row2_imu_failed_sets_and_recovers(void);
void test_policy_row3_corrupt_records_flags_and_reports_once(void);
void test_policy_row3_low_free_and_torn_growth_but_not_eviction(void);
void test_policy_row3_storage_task_stall_maps_flag_without_own_code(void);
void test_policy_row4_never_connected_never_flags(void);
void test_policy_row4_loss_after_connection_confirms_at_boundary(void);
void test_policy_row4_future_snapshot_is_unknown_not_down(void);
void test_policy_row4_held_event_time_snapshot_confirms_loss(void);
void test_policy_row4_missing_snapshot_breaks_confirmation(void);
void test_policy_row4_backward_time_restarts_confirmation(void);
void test_policy_row4_comms_task_stall_maps_flag_without_own_code(void);
void test_policy_row7_safe_mode_flagged_with_bootloop_cause(void);
void test_policy_rows9_10_battery_and_i2c_are_bits_only(void);
void test_policy_row5_tier_a_stall_is_not_a_facility(void);
void test_policy_row6_diag_ui_pump_stalls_are_bits_only(void);
void test_policy_absent_inputs_produce_no_flags(void);

// ---- no heap (spec §16.8 item 9) ---------------------------------------------
void test_no_heap_no_allocation_over_1000_steps(void);

// ---- wiring group (spec §16.8 items 6/7/10; shared statics — see above) ------
void test_wire_binds_registry_and_state_absent_before_tick(void);
void test_wire_sensor_status_mailbox_drives_current_capability(void);
void test_wire_watchdog_errors_remain_observable(void);
void test_wire_missing_boot_record_is_normal_boot(void);
void test_wire_streak_accumulates_to_safe_mode_and_latches_bootloop(void);
void test_wire_corrupt_boot_record_treated_as_absent(void);
void test_wire_run_tick_native_shape_is_honest(void);
void test_wire_tier_a_stall_reports_and_persists_crash_record(void);
void test_wire_tier_b_stalls_map_to_facility_flags(void);
void test_wire_reinit_seam_returns_distinct_ladders(void);
void test_wire_task_create_failure_latches_task_create_failed(void);
void test_wire_failed_boot_write_retries_at_bounded_cadence(void);
void test_wire_failed_survival_write_retries_before_reboot(void);
void test_wire_transient_boot_read_preserves_history_until_retry(void);
void test_state_provider_false_before_first_tick(void);
void test_state_provider_propagates_all_fields(void);
void test_state_provider_is_superset_of_f23_local_bits(void);
void test_events_enter_edge_emits_one_fault_event(void);
void test_events_no_repeat_while_flag_held(void);
void test_events_rate_limit_then_reedge_after_window(void);
void test_events_anomalous_boot_emits_once(void);
void test_events_ordinary_boot_emits_nothing(void);
void test_events_dropped_publishes_are_counted(void);
void test_events_dropped_publish_reports_event_publish_drop(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_registry_first_report_fills_empty_state);
    RUN_TEST(test_registry_counts_first_last_and_max_severity);
    RUN_TEST(test_registry_sticky_bits_follow_bit_n_code_n);
    RUN_TEST(test_registry_none_and_out_of_range_are_noops);
    RUN_TEST(test_registry_cr033_codes_are_in_range);
    RUN_TEST(test_registry_bits_are_sticky);
    RUN_TEST(test_registry_interleaved_reports_stay_per_code);
    RUN_TEST(test_registry_works_through_iface_pointer);

    RUN_TEST(test_supervisor_all_fresh_feeds);
    RUN_TEST(test_supervisor_tier_a_stall_withholds_feed);
    RUN_TEST(test_supervisor_tier_b_stall_flags_but_keeps_feeding);
    RUN_TEST(test_supervisor_grace_window_feeds_pending_tasks);
    RUN_TEST(test_supervisor_pending_after_grace_does_not_feed);
    RUN_TEST(test_supervisor_nothing_configured_never_feeds);
    RUN_TEST(test_supervisor_newly_stalled_fires_once_per_episode);
    RUN_TEST(test_supervisor_clock_jumps_are_safe);
    RUN_TEST(test_supervisor_configure_is_argument_safe);
    RUN_TEST(test_supervisor_rejects_sentinel_and_invalid_task_ids);

    RUN_TEST(test_boot_two_fast_boots_normal_third_safe_mode);
    RUN_TEST(test_boot_survival_clears_streak_once_safe_mode_stays);
    RUN_TEST(test_boot_streak_saturates);
    RUN_TEST(test_boot_reset_reason_maps_to_row8_codes);
    RUN_TEST(test_boot_backwards_clock_never_clears);

    RUN_TEST(test_reinit_first_attempt_is_due_immediately);
    RUN_TEST(test_reinit_backoff_doubles_to_cap);
    RUN_TEST(test_reinit_not_due_before_horizon);
    RUN_TEST(test_reinit_ok_cancels_and_resets_backoff);

    RUN_TEST(test_crash_record_round_trip);
    RUN_TEST(test_crash_record_encode_small_buffer_writes_nothing);
    RUN_TEST(test_crash_record_schema_mismatch_is_absent);
    RUN_TEST(test_crash_record_truncated_or_corrupt_is_absent);

    RUN_TEST(test_policy_row1_ppg_failed_sets_flag_with_edge);
    RUN_TEST(test_policy_row1_recovery_clears_flag_despite_sticky_bits);
    RUN_TEST(test_policy_row1_absent_status_uses_latched_codes);
    RUN_TEST(test_policy_row2_imu_failed_sets_and_recovers);
    RUN_TEST(test_policy_row3_corrupt_records_flags_and_reports_once);
    RUN_TEST(test_policy_row3_low_free_and_torn_growth_but_not_eviction);
    RUN_TEST(test_policy_row3_storage_task_stall_maps_flag_without_own_code);
    RUN_TEST(test_policy_row4_never_connected_never_flags);
    RUN_TEST(test_policy_row4_loss_after_connection_confirms_at_boundary);
    RUN_TEST(test_policy_row4_future_snapshot_is_unknown_not_down);
    RUN_TEST(test_policy_row4_held_event_time_snapshot_confirms_loss);
    RUN_TEST(test_policy_row4_missing_snapshot_breaks_confirmation);
    RUN_TEST(test_policy_row4_backward_time_restarts_confirmation);
    RUN_TEST(test_policy_row4_comms_task_stall_maps_flag_without_own_code);
    RUN_TEST(test_policy_row7_safe_mode_flagged_with_bootloop_cause);
    RUN_TEST(test_policy_rows9_10_battery_and_i2c_are_bits_only);
    RUN_TEST(test_policy_row5_tier_a_stall_is_not_a_facility);
    RUN_TEST(test_policy_row6_diag_ui_pump_stalls_are_bits_only);
    RUN_TEST(test_policy_absent_inputs_produce_no_flags);

    RUN_TEST(test_no_heap_no_allocation_over_1000_steps);

    RUN_TEST(test_wire_binds_registry_and_state_absent_before_tick);
    RUN_TEST(test_wire_sensor_status_mailbox_drives_current_capability);
    RUN_TEST(test_wire_watchdog_errors_remain_observable);
    RUN_TEST(test_wire_missing_boot_record_is_normal_boot);
    RUN_TEST(test_wire_streak_accumulates_to_safe_mode_and_latches_bootloop);
    RUN_TEST(test_wire_corrupt_boot_record_treated_as_absent);
    RUN_TEST(test_wire_run_tick_native_shape_is_honest);
    RUN_TEST(test_wire_tier_a_stall_reports_and_persists_crash_record);
    RUN_TEST(test_wire_tier_b_stalls_map_to_facility_flags);
    RUN_TEST(test_wire_reinit_seam_returns_distinct_ladders);
    RUN_TEST(test_wire_task_create_failure_latches_task_create_failed);
    RUN_TEST(test_wire_failed_boot_write_retries_at_bounded_cadence);
    RUN_TEST(test_wire_failed_survival_write_retries_before_reboot);
    RUN_TEST(test_wire_transient_boot_read_preserves_history_until_retry);
    RUN_TEST(test_state_provider_false_before_first_tick);
    RUN_TEST(test_state_provider_propagates_all_fields);
    RUN_TEST(test_state_provider_is_superset_of_f23_local_bits);
    RUN_TEST(test_events_enter_edge_emits_one_fault_event);
    RUN_TEST(test_events_no_repeat_while_flag_held);
    RUN_TEST(test_events_rate_limit_then_reedge_after_window);
    RUN_TEST(test_events_anomalous_boot_emits_once);
    RUN_TEST(test_events_ordinary_boot_emits_nothing);
    RUN_TEST(test_events_dropped_publishes_are_counted);
    RUN_TEST(test_events_dropped_publish_reports_event_publish_drop);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
