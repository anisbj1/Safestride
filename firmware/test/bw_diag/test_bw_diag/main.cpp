// Test runner for the bw_diag / F23 suite (spec §15.5).
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_state/test_bw_state/main.cpp.)
//
// ORDER MATTERS in the wire_diag group: the static output mailboxes are
// process-wide, so the "false before the first tick" seam test must run
// before any test that calls f23::runTick. The no-heap group MUST run
// LAST (after wire_diag): its registry-RMW loop notes every slot, which
// would break the wire group's "all-invalid before first note" and
// "native shape" assertions if it ran first.

#include <unity.h>

// ---- quality hysteresis (spec §9, D7) ---------------------------------------
void test_hysteresis_boot_is_insufficient_fail_safe(void);
void test_hysteresis_degraded_enters_after_enter_windows(void);
void test_hysteresis_single_bad_window_no_change(void);
void test_hysteresis_good_recovers_after_recover_windows(void);
void test_hysteresis_insufficient_enters_immediately(void);
void test_hysteresis_insufficient_to_good_needs_full_recovery(void);
void test_hysteresis_flapping_settles_at_degraded(void);
void test_hysteresis_reset_returns_to_fail_safe(void);

// ---- IMU assessor (spec §9, §15.5 item 2; §15.3 row ImuStuckSensor) ---------
void test_imu_healthy_frame_is_good(void);
void test_imu_failed_status_insufficient_no_f23_edge(void);
void test_imu_no_features_insufficient(void);
void test_imu_stale_features_insufficient(void);
void test_imu_huge_time_gap_is_stale(void);
void test_imu_identical_frames_x5_stuck_edge_once(void);
void test_imu_identical_frames_x4_no_fault(void);
void test_imu_raw_run_250_faults_249_does_not(void);
void test_imu_raw_recovery_allows_reedge(void);
void test_imu_absent_raw_window_ends_stuck_episode(void);
void test_imu_raw_episode_reedges_after_absent_gap(void);
void test_imu_stale_features_plus_raw_window(void);
void test_imu_first_call_empty_state(void);
void test_imu_f02_hint_degraded_worst_of(void);
void test_imu_degraded_status_worst_of(void);

// ---- PPG assessor (spec §9, §15.5 item 3; §15.3 rows Ppg*) ------------------
void test_ppg_healthy_is_good(void);
void test_ppg_bench_absent_no_f23_edge(void);
void test_ppg_status_ok_no_features_insufficient(void);
void test_ppg_poor_contact_pi_0_001_warns(void);
void test_ppg_poor_contact_none_at_pi_0_003(void);
void test_ppg_flatline_identical_dc_x5_low_pi(void);
void test_ppg_constant_dc_with_perfusion_is_not_flatline(void);
void test_ppg_raw_saturated_2pct_faults_1_9pct_does_not(void);
void test_ppg_raw_flatline_250_faults_249_does_not(void);
void test_ppg_raw_empty_window_not_evaluated(void);
void test_ppg_f02_hint_worst_of(void);
void test_ppg_stale_features_raw_overrides_staleness(void);
void test_ppg_motion_corrupted_at_least_degraded(void);
void test_ppg_degraded_status_worst_of(void);
void test_ppg_degraded_status_stale_features_raw_worst_of(void);
void test_ppg_poor_contact_reedges_after_clear(void);

// ---- system health (spec §9, §15.5 item 4; §15.3 health rows + D8) ----------
void test_health_invalid_battery_is_flag_only(void);
void test_health_battery_25_none(void);
void test_health_low_battery_19_9_warns(void);
void test_health_low_battery_boundary_20_0_silent(void);
void test_health_critical_7_9_fires_both(void);
void test_health_critical_boundary_8_0_low_only(void);
void test_health_low_heap_16383_vs_16384(void);
void test_health_overrun_51_vs_50(void);
void test_health_invalid_stats_no_faults(void);
void test_health_no_rereport_while_condition_holds(void);
void test_health_recharge_then_drop_reedges(void);
void test_health_degraded_mode_definition(void);
void test_health_worst_lateness_max_over_valid_slots(void);
void test_health_invalid_slots_excluded_from_lateness(void);
void test_health_legacy_stats_field_floors_max(void);
void test_health_overrun_via_tasks_array_51_50(void);
void test_health_low_stack_511_vs_512(void);
void test_health_low_stack_multi_task_single_edge_per_episode(void);
void test_health_low_stack_unmeasured_zero_silent(void);

// ---- composer (spec §9, §15.5 item 5; D8/D9, §10, §11) ----------------------
void test_diag_fail_safe_empty_input(void);
void test_diag_healthy_timeline_reaches_good(void);
void test_diag_composition_and_edge_cap(void);
void test_diag_rate_limit_suppresses_flapping_reedge(void);
void test_diag_sticky_fault_bits(void);
void test_diag_out_of_range_fault_code_is_noop(void);
void test_diag_degraded_mode_on_imu_loss(void);
void test_diag_end_to_end_staleness_hysteresis_timeline(void);
void test_diag_sizes_within_budget(void);
void test_diag_fault_bits_own_union_and_vanishing_provider(void);
void test_diag_degraded_mode_matrix(void);
void test_diag_cross_tick_raw_windows_reach_stuck(void);
void test_diag_absent_window_between_raw_windows_restarts_run(void);
void test_diag_low_stack_edge_reported_and_rate_limited(void);

// ---- task registry pure helpers (plan §16.2/§16.4, D13/D14) -----------------
void test_task_registry_healthy_array(void);
void test_task_registry_invalid_slots_excluded(void);
void test_task_registry_all_invalid_no_evidence(void);
void test_task_registry_empty_and_first_call(void);
void test_task_registry_slot_values(void);

// ---- no heap (spec §15.5 item 6) --------------------------------------------
void test_no_heap_no_allocation_over_1000_steps(void);
void test_no_heap_registry_rmw_and_raw_drain_1000(void);

// ---- wiring seam (plan D5/§15.6; DoD §4.8) ----------------------------------
void test_wire_diag_accessors_false_before_first_tick(void);
void test_wire_diag_gather_input_native_shape(void);
void test_wire_diag_tick_publishes_mailboxes_and_reports(void);
void test_wire_diag_null_sink_drops_edges(void);
void test_wire_diag_task_registry_round_trip(void);
void test_wire_diag_latest_task_health_false_before_first_note(void);
void test_wire_diag_raw_ingest_windows_present(void);
void test_wire_diag_drop_hole_suppresses_window(void);
void test_wire_diag_stack_watermark_unit_is_bytes(void);
void test_wire_diag_sensor_status_mailbox_tracks_recovery(void);
void test_wire_diag_drop_during_drain_suppresses_window(void);
void test_wire_diag_creation_failure_reports_own_code(void);
void test_wire_diag_reliability_provider_value_copy_and_detach(void);
void test_diag_held_feature_frames_are_not_new_stuck_evidence(void);
void test_diag_ppg_raw_flatline_spans_bounded_windows_and_resets_on_hole(void);
void test_diag_imu_raw_threshold_inside_window_is_not_lost_at_tail(void);
void test_diag_missing_evidence_breaks_stuck_continuity(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_hysteresis_boot_is_insufficient_fail_safe);
    RUN_TEST(test_hysteresis_degraded_enters_after_enter_windows);
    RUN_TEST(test_hysteresis_single_bad_window_no_change);
    RUN_TEST(test_hysteresis_good_recovers_after_recover_windows);
    RUN_TEST(test_hysteresis_insufficient_enters_immediately);
    RUN_TEST(test_hysteresis_insufficient_to_good_needs_full_recovery);
    RUN_TEST(test_hysteresis_flapping_settles_at_degraded);
    RUN_TEST(test_hysteresis_reset_returns_to_fail_safe);

    RUN_TEST(test_imu_healthy_frame_is_good);
    RUN_TEST(test_imu_failed_status_insufficient_no_f23_edge);
    RUN_TEST(test_imu_no_features_insufficient);
    RUN_TEST(test_imu_stale_features_insufficient);
    RUN_TEST(test_imu_huge_time_gap_is_stale);
    RUN_TEST(test_imu_identical_frames_x5_stuck_edge_once);
    RUN_TEST(test_imu_identical_frames_x4_no_fault);
    RUN_TEST(test_imu_raw_run_250_faults_249_does_not);
    RUN_TEST(test_imu_raw_recovery_allows_reedge);
    RUN_TEST(test_imu_absent_raw_window_ends_stuck_episode);
    RUN_TEST(test_imu_raw_episode_reedges_after_absent_gap);
    RUN_TEST(test_imu_stale_features_plus_raw_window);
    RUN_TEST(test_imu_first_call_empty_state);
    RUN_TEST(test_imu_f02_hint_degraded_worst_of);
    RUN_TEST(test_imu_degraded_status_worst_of);

    RUN_TEST(test_ppg_healthy_is_good);
    RUN_TEST(test_ppg_bench_absent_no_f23_edge);
    RUN_TEST(test_ppg_status_ok_no_features_insufficient);
    RUN_TEST(test_ppg_poor_contact_pi_0_001_warns);
    RUN_TEST(test_ppg_poor_contact_none_at_pi_0_003);
    RUN_TEST(test_ppg_flatline_identical_dc_x5_low_pi);
    RUN_TEST(test_ppg_constant_dc_with_perfusion_is_not_flatline);
    RUN_TEST(test_ppg_raw_saturated_2pct_faults_1_9pct_does_not);
    RUN_TEST(test_ppg_raw_flatline_250_faults_249_does_not);
    RUN_TEST(test_ppg_raw_empty_window_not_evaluated);
    RUN_TEST(test_ppg_f02_hint_worst_of);
    RUN_TEST(test_ppg_stale_features_raw_overrides_staleness);
    RUN_TEST(test_ppg_motion_corrupted_at_least_degraded);
    RUN_TEST(test_ppg_degraded_status_worst_of);
    RUN_TEST(test_ppg_degraded_status_stale_features_raw_worst_of);
    RUN_TEST(test_ppg_poor_contact_reedges_after_clear);

    RUN_TEST(test_health_invalid_battery_is_flag_only);
    RUN_TEST(test_health_battery_25_none);
    RUN_TEST(test_health_low_battery_19_9_warns);
    RUN_TEST(test_health_low_battery_boundary_20_0_silent);
    RUN_TEST(test_health_critical_7_9_fires_both);
    RUN_TEST(test_health_critical_boundary_8_0_low_only);
    RUN_TEST(test_health_low_heap_16383_vs_16384);
    RUN_TEST(test_health_overrun_51_vs_50);
    RUN_TEST(test_health_invalid_stats_no_faults);
    RUN_TEST(test_health_no_rereport_while_condition_holds);
    RUN_TEST(test_health_recharge_then_drop_reedges);
    RUN_TEST(test_health_degraded_mode_definition);
    RUN_TEST(test_health_worst_lateness_max_over_valid_slots);
    RUN_TEST(test_health_invalid_slots_excluded_from_lateness);
    RUN_TEST(test_health_legacy_stats_field_floors_max);
    RUN_TEST(test_health_overrun_via_tasks_array_51_50);
    RUN_TEST(test_health_low_stack_511_vs_512);
    RUN_TEST(test_health_low_stack_multi_task_single_edge_per_episode);
    RUN_TEST(test_health_low_stack_unmeasured_zero_silent);

    RUN_TEST(test_diag_fail_safe_empty_input);
    RUN_TEST(test_diag_healthy_timeline_reaches_good);
    RUN_TEST(test_diag_composition_and_edge_cap);
    RUN_TEST(test_diag_rate_limit_suppresses_flapping_reedge);
    RUN_TEST(test_diag_sticky_fault_bits);
    RUN_TEST(test_diag_out_of_range_fault_code_is_noop);
    RUN_TEST(test_diag_degraded_mode_on_imu_loss);
    RUN_TEST(test_diag_end_to_end_staleness_hysteresis_timeline);
    RUN_TEST(test_diag_sizes_within_budget);
    RUN_TEST(test_diag_fault_bits_own_union_and_vanishing_provider);
    RUN_TEST(test_diag_degraded_mode_matrix);
    RUN_TEST(test_diag_cross_tick_raw_windows_reach_stuck);
    RUN_TEST(test_diag_absent_window_between_raw_windows_restarts_run);
    RUN_TEST(test_diag_low_stack_edge_reported_and_rate_limited);

    RUN_TEST(test_task_registry_healthy_array);
    RUN_TEST(test_task_registry_invalid_slots_excluded);
    RUN_TEST(test_task_registry_all_invalid_no_evidence);
    RUN_TEST(test_task_registry_empty_and_first_call);
    RUN_TEST(test_task_registry_slot_values);

    RUN_TEST(test_wire_diag_accessors_false_before_first_tick);
    RUN_TEST(test_wire_diag_gather_input_native_shape);
    RUN_TEST(test_wire_diag_tick_publishes_mailboxes_and_reports);
    RUN_TEST(test_wire_diag_null_sink_drops_edges);
    RUN_TEST(test_wire_diag_task_registry_round_trip);
    RUN_TEST(test_wire_diag_latest_task_health_false_before_first_note);
    RUN_TEST(test_wire_diag_raw_ingest_windows_present);
    RUN_TEST(test_wire_diag_drop_hole_suppresses_window);
    RUN_TEST(test_wire_diag_stack_watermark_unit_is_bytes);
    RUN_TEST(test_wire_diag_sensor_status_mailbox_tracks_recovery);
    RUN_TEST(test_wire_diag_drop_during_drain_suppresses_window);
    RUN_TEST(test_wire_diag_creation_failure_reports_own_code);
    RUN_TEST(test_wire_diag_reliability_provider_value_copy_and_detach);
    RUN_TEST(test_diag_held_feature_frames_are_not_new_stuck_evidence);
    RUN_TEST(test_diag_ppg_raw_flatline_spans_bounded_windows_and_resets_on_hole);
    RUN_TEST(test_diag_imu_raw_threshold_inside_window_is_not_lost_at_tail);
    RUN_TEST(test_diag_missing_evidence_breaks_stuck_continuity);

    // LAST: these note every registry slot, which the wire group's
    // "before first note"/"native shape" assertions depend on not having
    // happened yet (see the order comment at the top of this file).
    RUN_TEST(test_no_heap_no_allocation_over_1000_steps);
    RUN_TEST(test_no_heap_registry_rmw_and_raw_drain_1000);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
