// Test runner for the bw_eventlog / F17 suite (§9, §15.6).
//
// PlatformIO builds every .cpp in this directory into one program and does NOT
// generate a Unity main, so this file owns it. REGISTER EVERY test function
// here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_sleep/test_bw_sleep/test_main.cpp.)
//
// Ordering note: the wiring tests share ONE process-wide static EventLogger
// (loggerImpl's function-local static, constructed on the first f17::wire with
// whatever AppContext it is handed). The full-path test therefore runs BEFORE
// the empty-context smoke test, so the logger is built with a real clock and
// the later wire(nullptr clock) call is the documented idempotent no-op.

#include <unity.h>

// ---- seq / boot_id recovery (spec §9a) ---------------------------------------
void test_seq_first_append_is_one_and_fanned_out(void);
void test_seq_monotonic_in_order_across_drains(void);
void test_seq_continues_across_simulated_reboot(void);
void test_bootid_recovers_from_preseeded_tail(void);
void test_empty_store_hint_recovers_without_read(void);
void test_valid_tail_hint_reads_once_and_increments_boot_id(void);
void test_bad_tail_hint_falls_back_to_generic_recovery(void);
void test_no_hint_retains_generic_recovery(void);
void test_failed_append_consumes_no_seq(void);
void test_store_absent_holds_queue_without_fanout(void);
void test_late_store_attach_recovers_then_drains(void);
void test_recovery_read_error_defaults_to_first_boot(void);
void test_reattach_does_not_reprobe_or_skip_boot(void);
void test_fanned_event_matches_store_record(void);

// ---- de-dup / rate limiting (spec §9b, §15.2 D7) ----------------------------
void test_rate_sensor_fault_suppressed_inside_window(void);
void test_rate_sensor_fault_allowed_at_exact_interval(void);
void test_rate_boundary_one_ms_inside_is_suppressed(void);
void test_rate_device_not_worn_5000_window(void);
void test_rate_battery_warning_60000_window(void);
void test_rate_unlimited_type_never_suppressed(void);
void test_rate_per_type_independence(void);
void test_rate_no_time_reference_passes_through(void);
void test_rate_storm_cannot_fill_queue(void);

// ---- priority overflow policy (spec §9b, §6 D2) -----------------------------
void test_overflow_equal_priority_refuses_incoming(void);
void test_overflow_rejected_event_leaves_queue_untouched(void);
void test_overflow_critical_evicts_oldest_normal_order_preserved(void);
void test_overflow_high_evicts_normal(void);
void test_overflow_normal_rejected_when_all_critical(void);
void test_overflow_critical_rejected_when_queue_all_critical(void);
void test_overflow_critical_resident_survives_high_incoming(void);
void test_overflow_victim_is_oldest_of_least_important(void);
void test_overflow_high_water_recorded(void);
void test_overflow_drain_frees_capacity(void);
void test_overflow_eviction_during_append_never_orphans_a_queued_event(void);

// ---- the §15.3 table itself (DoD §14) ---------------------------------------
void test_priority_all_twelve_rows_match_spec(void);
void test_priority_critical_set_is_exactly_the_five(void);
void test_priority_unknown_future_type_defaults_normal(void);
void test_priority_registry_and_constant_pins(void);

// ---- wall time + F17-generated records (spec §9c, §15.2 D10) ----------------
void test_boot_emitted_once_with_detector_17(void);
void test_boot_t_start_ms_filled_from_clock(void);
void test_wall_unknown_stays_zero_into_store(void);
void test_wall_known_stamps_at_publish(void);
void test_wall_producer_captured_value_preserved(void);
void test_timesync_emitted_once_when_wall_becomes_known(void);
void test_timesync_static_wall_clock_never_repeats(void);
void test_timesync_ticking_clock_never_emits(void);
void test_timesync_jump_beyond_tolerance_emits(void);
void test_timesync_backwards_set_emits(void);
void test_wall_time_without_clock_never_crashes(void);
void test_wall_unknown_at_publish_stays_zero_when_clock_syncs_before_drain(void);

// ---- fan-out to subscribers (spec §6 D1, §15.2 D4) --------------------------
void test_fanout_carries_final_seq_matching_store(void);
void test_fanout_order_matches_append_order(void);
void test_fanout_capacity_eight_ninth_rejected(void);
void test_fanout_null_and_duplicate_rejected(void);
void test_fanout_rejecting_subscriber_counted_others_served(void);
void test_fanout_without_subscribers_still_appends(void);
void test_fanout_subscriber_sees_every_drain(void);

// ---- store failure / backoff (spec §10, §15.2 D8) ---------------------------
void test_store_failure_keeps_head_reports_once(void);
void test_store_failure_backoff_gates_retry(void);
void test_store_failure_recovery_preserves_order_and_seq(void);
void test_store_failure_new_episode_reports_again(void);
void test_store_failure_publish_returns_ok_during_outage(void);
void test_store_failure_overflow_policy_active_during_outage(void);
void test_store_failure_without_reporter_is_safe(void);
void test_store_failure_full_queue_heals_in_order(void);

// ---- record stamping (spec §15.2 D10) ---------------------------------------
void test_stamping_schema_version_always_current(void);
void test_stamping_overwrites_stale_schema_version(void);
void test_stamping_t_start_ms_filled_only_when_zero(void);
void test_stamping_producer_owned_fields_pass_through(void);
void test_stamping_t_start_unix_filled_when_clock_synced(void);

// ---- wiring sanity (additive; README §3 native-compile rule) ----------------
void test_wiring_constants_match_spec(void);
void test_wiring_full_path_appends_and_fans_out(void);
void test_wiring_wire_is_safe_with_empty_context(void);
void test_wiring_wire_claims_pump_slot_exactly_once(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_seq_first_append_is_one_and_fanned_out);
    RUN_TEST(test_seq_monotonic_in_order_across_drains);
    RUN_TEST(test_seq_continues_across_simulated_reboot);
    RUN_TEST(test_bootid_recovers_from_preseeded_tail);
    RUN_TEST(test_empty_store_hint_recovers_without_read);
    RUN_TEST(test_valid_tail_hint_reads_once_and_increments_boot_id);
    RUN_TEST(test_bad_tail_hint_falls_back_to_generic_recovery);
    RUN_TEST(test_no_hint_retains_generic_recovery);
    RUN_TEST(test_failed_append_consumes_no_seq);
    RUN_TEST(test_store_absent_holds_queue_without_fanout);
    RUN_TEST(test_late_store_attach_recovers_then_drains);
    RUN_TEST(test_recovery_read_error_defaults_to_first_boot);
    RUN_TEST(test_reattach_does_not_reprobe_or_skip_boot);
    RUN_TEST(test_fanned_event_matches_store_record);

    RUN_TEST(test_rate_sensor_fault_suppressed_inside_window);
    RUN_TEST(test_rate_sensor_fault_allowed_at_exact_interval);
    RUN_TEST(test_rate_boundary_one_ms_inside_is_suppressed);
    RUN_TEST(test_rate_device_not_worn_5000_window);
    RUN_TEST(test_rate_battery_warning_60000_window);
    RUN_TEST(test_rate_unlimited_type_never_suppressed);
    RUN_TEST(test_rate_per_type_independence);
    RUN_TEST(test_rate_no_time_reference_passes_through);
    RUN_TEST(test_rate_storm_cannot_fill_queue);

    RUN_TEST(test_overflow_equal_priority_refuses_incoming);
    RUN_TEST(test_overflow_rejected_event_leaves_queue_untouched);
    RUN_TEST(test_overflow_critical_evicts_oldest_normal_order_preserved);
    RUN_TEST(test_overflow_high_evicts_normal);
    RUN_TEST(test_overflow_normal_rejected_when_all_critical);
    RUN_TEST(test_overflow_critical_rejected_when_queue_all_critical);
    RUN_TEST(test_overflow_critical_resident_survives_high_incoming);
    RUN_TEST(test_overflow_victim_is_oldest_of_least_important);
    RUN_TEST(test_overflow_high_water_recorded);
    RUN_TEST(test_overflow_drain_frees_capacity);
    RUN_TEST(test_overflow_eviction_during_append_never_orphans_a_queued_event);

    RUN_TEST(test_priority_all_twelve_rows_match_spec);
    RUN_TEST(test_priority_critical_set_is_exactly_the_five);
    RUN_TEST(test_priority_unknown_future_type_defaults_normal);
    RUN_TEST(test_priority_registry_and_constant_pins);

    RUN_TEST(test_boot_emitted_once_with_detector_17);
    RUN_TEST(test_boot_t_start_ms_filled_from_clock);
    RUN_TEST(test_wall_unknown_stays_zero_into_store);
    RUN_TEST(test_wall_known_stamps_at_publish);
    RUN_TEST(test_wall_producer_captured_value_preserved);
    RUN_TEST(test_timesync_emitted_once_when_wall_becomes_known);
    RUN_TEST(test_timesync_static_wall_clock_never_repeats);
    RUN_TEST(test_timesync_ticking_clock_never_emits);
    RUN_TEST(test_timesync_jump_beyond_tolerance_emits);
    RUN_TEST(test_timesync_backwards_set_emits);
    RUN_TEST(test_wall_time_without_clock_never_crashes);
    RUN_TEST(test_wall_unknown_at_publish_stays_zero_when_clock_syncs_before_drain);

    RUN_TEST(test_fanout_carries_final_seq_matching_store);
    RUN_TEST(test_fanout_order_matches_append_order);
    RUN_TEST(test_fanout_capacity_eight_ninth_rejected);
    RUN_TEST(test_fanout_null_and_duplicate_rejected);
    RUN_TEST(test_fanout_rejecting_subscriber_counted_others_served);
    RUN_TEST(test_fanout_without_subscribers_still_appends);
    RUN_TEST(test_fanout_subscriber_sees_every_drain);

    RUN_TEST(test_store_failure_keeps_head_reports_once);
    RUN_TEST(test_store_failure_backoff_gates_retry);
    RUN_TEST(test_store_failure_recovery_preserves_order_and_seq);
    RUN_TEST(test_store_failure_new_episode_reports_again);
    RUN_TEST(test_store_failure_publish_returns_ok_during_outage);
    RUN_TEST(test_store_failure_overflow_policy_active_during_outage);
    RUN_TEST(test_store_failure_without_reporter_is_safe);
    RUN_TEST(test_store_failure_full_queue_heals_in_order);

    RUN_TEST(test_stamping_schema_version_always_current);
    RUN_TEST(test_stamping_overwrites_stale_schema_version);
    RUN_TEST(test_stamping_t_start_ms_filled_only_when_zero);
    RUN_TEST(test_stamping_producer_owned_fields_pass_through);
    RUN_TEST(test_stamping_t_start_unix_filled_when_clock_synced);

    RUN_TEST(test_wiring_constants_match_spec);
    RUN_TEST(test_wiring_full_path_appends_and_fans_out);
    RUN_TEST(test_wiring_wire_is_safe_with_empty_context);
    RUN_TEST(test_wiring_wire_claims_pump_slot_exactly_once);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
