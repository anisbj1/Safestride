// Test runner for the bw_history / F16 suite (spec §9, T1–T12).
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_sleep/test_bw_sleep/test_main.cpp.)

#include <unity.h>

// ---- T1: basic aggregation (spec §9, §6 D5–D7) -------------------------------
void test_aggregation_basic(void);
void test_aggregation_empty_store_is_honest(void);
void test_foreign_detector_episode_not_counted(void);  // D7 detector-8 filter

// ---- T2: day boundary and daytime window (§9, §6 D5/D6) ---------------------
void test_day_boundary_night_episode_belongs_to_start_day(void);
void test_day_boundary_exact_boundary_instants(void);
void test_day_boundary_daytime_window_edges(void);
void test_day_boundary_multi_day_episode_keeps_full_duration(void);
void test_day_boundary_midnight_wall_is_same_logical_day(void);

// ---- T3: unknown wall time and derivation (§9, §6 D4, §10) -------------------
void test_unknown_wall_time_session_summary_per_boot(void);
void test_unknown_wall_time_session_finalizes_on_next_boot(void);
void test_clock_derivation_within_current_boot(void);
void test_other_boot_never_derived(void);
void test_unknown_then_synced_switches_identity(void);
void test_stale_boot_event_never_fabricates_day(void);    // audit A2
void test_missing_boot_event_never_fabricates_day(void);  // audit A2

// ---- T4 + T11: persistence, errors, corruption (§9, §6 D3, §10) --------------
void test_boot_restore_is_bounded_and_hides_summaries_until_ready(void);
void test_cursor_persisted_across_reboot_no_double_count(void);
void test_cursor_persisted_single_event_stays_open(void);
void test_read_error_skips_tick_and_retries(void);
void test_kv_corrupt_slot_discard_and_full_rescan(void);
void test_kv_corrupt_meta_discard_and_full_rescan(void);
void test_kv_unknown_meta_schema_discard(void);
void test_kv_read_error_fails_boot_without_discarding(void);  // audit A1
void test_kv_slot_write_failure_retries_finalize(void);       // §10 write path 1
void test_kv_meta_write_failure_lands_day_once(void);         // §10 write path 2
void test_seq_exhaustion_cursor_never_wraps(void);            // audit: seq guard

// ---- T5: reproducibility from events alone (§9, DoD §14) --------------------
void test_reproducibility_incremental_vs_full_recompute(void);
void test_reproducibility_after_label_change_and_reboot(void);
void test_reproducibility_recovered_episode_is_zero_ms(void);

// ---- T6: clock regression (§9, §6 D9, §10) -----------------------------------
void test_clock_regression_backward_jump_reopens_and_counts(void);
void test_clock_regression_forward_jump_no_synthetic_days(void);
void test_clock_regression_session_day_interleaves_cleanly(void);
void test_clock_regression_persisted_on_last_finalize(void);  // audit A4

// ---- T7: ring eviction (§9, §8, §6 D1) ---------------------------------------
void test_ring_eviction_keeps_newest_seven(void);
void test_ring_eviction_session_records_occupy_slots(void);
void test_full_ring_crash_window_replay_no_duplicate(void);  // crash-window dedupe

// ---- T8: confirmed refresh at boot (§9, §6 D8) -------------------------------
void test_confirmed_refresh_within_rebuild_window(void);
void test_confirmed_refresh_older_day_stays_stale(void);
void test_confirmed_refresh_live_day_not_lost_by_reboot(void);

// ---- T9: no heap (§9, §6 D10, 00 §7.2) ---------------------------------------
void test_no_heap_no_allocation_over_1000_ticks(void);

// ---- T10: incremental batching (§9, §6 D3) -----------------------------------
void test_incremental_batching_backlog_matches_oneshot(void);
void test_incremental_batching_batch_size_is_bounded(void);

// ---- T12: wiring compile + degradation (§9, §10) -----------------------------
void test_wiring_constants_match_spec(void);
void test_wiring_tick_before_init_is_notready(void);  // before wireHistoryInit()
void test_wiring_null_providers_degrade_without_crash(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_aggregation_basic);
    RUN_TEST(test_aggregation_empty_store_is_honest);
    RUN_TEST(test_foreign_detector_episode_not_counted);

    RUN_TEST(test_day_boundary_night_episode_belongs_to_start_day);
    RUN_TEST(test_day_boundary_exact_boundary_instants);
    RUN_TEST(test_day_boundary_daytime_window_edges);
    RUN_TEST(test_day_boundary_multi_day_episode_keeps_full_duration);
    RUN_TEST(test_day_boundary_midnight_wall_is_same_logical_day);

    RUN_TEST(test_unknown_wall_time_session_summary_per_boot);
    RUN_TEST(test_unknown_wall_time_session_finalizes_on_next_boot);
    RUN_TEST(test_clock_derivation_within_current_boot);
    RUN_TEST(test_other_boot_never_derived);
    RUN_TEST(test_unknown_then_synced_switches_identity);
    RUN_TEST(test_stale_boot_event_never_fabricates_day);
    RUN_TEST(test_missing_boot_event_never_fabricates_day);

    RUN_TEST(test_cursor_persisted_across_reboot_no_double_count);
    RUN_TEST(test_cursor_persisted_single_event_stays_open);
    RUN_TEST(test_read_error_skips_tick_and_retries);
    RUN_TEST(test_kv_corrupt_slot_discard_and_full_rescan);
    RUN_TEST(test_kv_corrupt_meta_discard_and_full_rescan);
    RUN_TEST(test_kv_unknown_meta_schema_discard);
    RUN_TEST(test_kv_read_error_fails_boot_without_discarding);
    RUN_TEST(test_kv_slot_write_failure_retries_finalize);
    RUN_TEST(test_kv_meta_write_failure_lands_day_once);
    RUN_TEST(test_seq_exhaustion_cursor_never_wraps);
    RUN_TEST(test_boot_restore_is_bounded_and_hides_summaries_until_ready);

    RUN_TEST(test_reproducibility_incremental_vs_full_recompute);
    RUN_TEST(test_reproducibility_after_label_change_and_reboot);
    RUN_TEST(test_reproducibility_recovered_episode_is_zero_ms);

    RUN_TEST(test_clock_regression_backward_jump_reopens_and_counts);
    RUN_TEST(test_clock_regression_forward_jump_no_synthetic_days);
    RUN_TEST(test_clock_regression_session_day_interleaves_cleanly);
    RUN_TEST(test_clock_regression_persisted_on_last_finalize);

    RUN_TEST(test_ring_eviction_keeps_newest_seven);
    RUN_TEST(test_ring_eviction_session_records_occupy_slots);
    RUN_TEST(test_full_ring_crash_window_replay_no_duplicate);

    RUN_TEST(test_confirmed_refresh_within_rebuild_window);
    RUN_TEST(test_confirmed_refresh_older_day_stays_stale);
    RUN_TEST(test_confirmed_refresh_live_day_not_lost_by_reboot);

    RUN_TEST(test_no_heap_no_allocation_over_1000_ticks);

    RUN_TEST(test_incremental_batching_backlog_matches_oneshot);
    RUN_TEST(test_incremental_batching_batch_size_is_bounded);

    RUN_TEST(test_wiring_constants_match_spec);
    RUN_TEST(test_wiring_tick_before_init_is_notready);
    RUN_TEST(test_wiring_null_providers_degrade_without_crash);

    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
