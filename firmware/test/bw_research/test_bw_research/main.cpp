// Test runner for the bw_research / F28 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_activity/test_bw_activity/main.cpp.)

#include <unity.h>

// ---- line format (spec §9.1) -------------------------------------------------
void test_header_golden(void);
void test_header_sanitises_firmware_version(void);
void test_header_null_version_is_unknown(void);
void test_header_truncates_long_version(void);
void test_imu_line_golden(void);
void test_ppg_line_golden(void);
void test_motion_feature_line_golden(void);
void test_ppg_feature_line_golden(void);
void test_context_line_golden(void);
void test_marker_line_golden(void);
void test_event_line_golden(void);
void test_cap_too_small_writes_nothing(void);
void test_cap_boundary_is_exact(void);
void test_event_worst_case_line_fits_max_line_len(void);
void test_activity_names_table(void);
void test_event_type_names_table(void);
void test_quality_names_table(void);

// ---- streamer (spec §9.2) ----------------------------------------------------
void test_gate_default_off_produces_nothing(void);
void test_gate_config_flag_on_streams(void);
void test_gate_research_mode_enables_without_flag(void);
void test_gate_day_mode_stays_off(void);
void test_gate_no_state_yet_stays_off(void);
void test_gate_disable_stops_rows(void);
void test_header_is_first_row_and_carries_identity(void);
void test_header_repeats_at_interval(void);
void test_motion_row_emitted_once_per_value(void);
void test_motion_row_updated_on_new_value(void);
void test_same_timestamp_not_emitted_twice(void);
void test_all_three_streams_produce_rows(void);
void test_empty_sources_yield_header_only(void);
void test_missed_rows_counted_on_sequence_jump(void);
void test_backpressure_defers_rows_and_counts_misses(void);
void test_drain_preserves_order_across_many_ticks(void);

// ---- raw tee (spec §9.3, D3) -------------------------------------------------
void test_tee_disabled_refuses_and_stores_nothing(void);
void test_tee_enabled_stores_in_order(void);
void test_tee_full_refuses_and_counts_drops(void);
void test_tee_gate_synced_from_streamer_activity(void);
void test_raw_rows_emitted_when_tee_enabled(void);
void test_raw_rows_absent_while_gate_off(void);
void test_raw_backpressure_counts_drops_and_reports_in_header(void);

// ---- event/marker tap (spec §9.4, D8) ----------------------------------------
void test_events_stored_before_session_are_not_replayed(void);
void test_new_event_emitted_exactly_once(void);
void test_user_marked_emits_event_and_marker_rows(void);
void test_event_rows_deferred_on_full_ring_are_not_lost(void);
void test_event_cursor_resets_for_a_new_activation(void);
void test_no_event_store_produces_no_event_rows(void);

// ---- tick loop / wiring body (spec §9.5) ------------------------------------
void test_tick_loop_captures_every_raw_sample(void);
void test_tick_loop_raw_rows_are_time_ordered(void);
void test_tick_loop_slow_drain_counts_drops_instead_of_stalling(void);
void test_tick_loop_header_carries_final_drop_counters(void);

// ---- producer fairness / round-robin (F2 FIX, test_fairness.cpp) ------------
void test_fairness_features_emit_while_raw_backlog_never_empties(void);
void test_fairness_rotation_emits_all_pending_raw_rows(void);
void test_fairness_every_tick_emits_pending_features_under_sustained_raw(void);

// ---- 48-byte Event layout vector (spec §9.7, DATASET_FORMAT.md §5) -----------
void test_event_size_is_pinned(void);
void test_event_memory_image_matches_vector(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_header_golden);
    RUN_TEST(test_header_sanitises_firmware_version);
    RUN_TEST(test_header_null_version_is_unknown);
    RUN_TEST(test_header_truncates_long_version);
    RUN_TEST(test_imu_line_golden);
    RUN_TEST(test_ppg_line_golden);
    RUN_TEST(test_motion_feature_line_golden);
    RUN_TEST(test_ppg_feature_line_golden);
    RUN_TEST(test_context_line_golden);
    RUN_TEST(test_marker_line_golden);
    RUN_TEST(test_event_line_golden);
    RUN_TEST(test_cap_too_small_writes_nothing);
    RUN_TEST(test_cap_boundary_is_exact);
    RUN_TEST(test_event_worst_case_line_fits_max_line_len);
    RUN_TEST(test_activity_names_table);
    RUN_TEST(test_event_type_names_table);
    RUN_TEST(test_quality_names_table);

    RUN_TEST(test_gate_default_off_produces_nothing);
    RUN_TEST(test_gate_config_flag_on_streams);
    RUN_TEST(test_gate_research_mode_enables_without_flag);
    RUN_TEST(test_gate_day_mode_stays_off);
    RUN_TEST(test_gate_no_state_yet_stays_off);
    RUN_TEST(test_gate_disable_stops_rows);
    RUN_TEST(test_header_is_first_row_and_carries_identity);
    RUN_TEST(test_header_repeats_at_interval);
    RUN_TEST(test_motion_row_emitted_once_per_value);
    RUN_TEST(test_motion_row_updated_on_new_value);
    RUN_TEST(test_same_timestamp_not_emitted_twice);
    RUN_TEST(test_all_three_streams_produce_rows);
    RUN_TEST(test_empty_sources_yield_header_only);
    RUN_TEST(test_missed_rows_counted_on_sequence_jump);
    RUN_TEST(test_backpressure_defers_rows_and_counts_misses);
    RUN_TEST(test_drain_preserves_order_across_many_ticks);

    RUN_TEST(test_tee_disabled_refuses_and_stores_nothing);
    RUN_TEST(test_tee_enabled_stores_in_order);
    RUN_TEST(test_tee_full_refuses_and_counts_drops);
    RUN_TEST(test_tee_gate_synced_from_streamer_activity);
    RUN_TEST(test_raw_rows_emitted_when_tee_enabled);
    RUN_TEST(test_raw_rows_absent_while_gate_off);
    RUN_TEST(test_raw_backpressure_counts_drops_and_reports_in_header);

    RUN_TEST(test_events_stored_before_session_are_not_replayed);
    RUN_TEST(test_new_event_emitted_exactly_once);
    RUN_TEST(test_user_marked_emits_event_and_marker_rows);
    RUN_TEST(test_event_rows_deferred_on_full_ring_are_not_lost);
    RUN_TEST(test_event_cursor_resets_for_a_new_activation);
    RUN_TEST(test_no_event_store_produces_no_event_rows);

    RUN_TEST(test_tick_loop_captures_every_raw_sample);
    RUN_TEST(test_tick_loop_raw_rows_are_time_ordered);
    RUN_TEST(test_tick_loop_slow_drain_counts_drops_instead_of_stalling);
    RUN_TEST(test_tick_loop_header_carries_final_drop_counters);

    RUN_TEST(test_fairness_features_emit_while_raw_backlog_never_empties);
    RUN_TEST(test_fairness_rotation_emits_all_pending_raw_rows);
    RUN_TEST(test_fairness_every_tick_emits_pending_features_under_sustained_raw);

    RUN_TEST(test_event_size_is_pinned);
    RUN_TEST(test_event_memory_image_matches_vector);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
