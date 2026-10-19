// Test runner for the bw_confirm / F14 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_cataplexy/test_bw_cataplexy/main.cpp.)
//
// The four test_wire_* cases share f14::manager()'s static singleton and are
// ORDER-DEPENDENT: register them last, in exactly the written order.

#include <unity.h>

// ---- scripted timelines (plan §9.1) ---------------------------------------
void test_timeline_event_to_confirm(void);
void test_timeline_event_to_reject(void);
void test_timeline_prompt_timeout_unlabeled(void);
void test_timeline_new_alert_withdraws_prompt(void);
void test_timeline_never_acks_stays_deferred(void);
void test_timeline_offer_at_exact_defer_boundary(void);

// ---- queue overflow + ring unit tests (plan §9.2) -------------------------
void test_overflow_drops_oldest_with_unlabeled_marker(void);
void test_overflow_drains_in_order_after_ack(void);
void test_overflow_keeps_active_prompt(void);
void test_withdraw_pushfront_evicts_oldest_waiting(void);
void test_queue_push_pop_preserves_fifo_order(void);
void test_queue_remove_by_seq_keeps_ring_order(void);

// ---- store labels + bounded retry (plan §9.3, spec §10) -------------------
void test_store_labels_captured_with_seq_and_label(void);
void test_store_failure_retried_then_succeeds(void);
void test_store_persistent_failure_gives_up(void);
void test_store_failure_does_not_block_next_prompt(void);
void test_label_notification_survives_store_failure(void);
void test_null_store_and_sink_count_drops(void);
void test_retry_buffer_full_gives_up_bounded(void);

// ---- late labeling (plan §9.4, D1) ----------------------------------------
void test_late_confirm_after_timeout_still_labels(void);
void test_late_reject_unknown_seq_labels_store(void);
void test_confirm_for_waiting_entry_resolves_and_removes(void);
void test_non_label_commands_not_consumed(void);
void test_button_confirm_with_arg0_zero_targets_offered(void);
void test_button_reject_with_arg0_zero_targets_offered(void);
void test_button_confirm_with_no_offered_prompt_is_noop(void);

// ---- CR-016 provider output (plan §9.5) -----------------------------------
void test_provider_false_before_first_step(void);
void test_provider_idle_after_first_step(void);
void test_provider_deferred_then_offered_fields(void);
void test_provider_idle_after_last_resolve(void);
void test_provider_schema_version(void);

// ---- label stats (plan §9.6, DoD §14) -------------------------------------
void test_stats_zero_initially(void);
void test_stats_mixed_timeline_counts(void);
void test_stats_publish_failure_counted(void);
void test_stats_manager_state_within_ram_budget(void);

// ---- eligibility policy (plan §9.7, D6) -----------------------------------
void test_ineligible_types_never_queued(void);
void test_own_detector_events_ignored(void);
void test_custom_eligibility_config(void);
void test_default_eligibility_matches_spec(void);

// ---- wiring seams (plan §15) — order-dependent, see file header -----------
void test_wire_manager_reachable(void);
void test_wire_step_null_clock_waits_honestly(void);
void test_wire_step_reads_alert_status_mailbox(void);
void test_wire_seams_forward_to_manager(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_timeline_event_to_confirm);
    RUN_TEST(test_timeline_event_to_reject);
    RUN_TEST(test_timeline_prompt_timeout_unlabeled);
    RUN_TEST(test_timeline_new_alert_withdraws_prompt);
    RUN_TEST(test_timeline_never_acks_stays_deferred);
    RUN_TEST(test_timeline_offer_at_exact_defer_boundary);

    RUN_TEST(test_overflow_drops_oldest_with_unlabeled_marker);
    RUN_TEST(test_overflow_drains_in_order_after_ack);
    RUN_TEST(test_overflow_keeps_active_prompt);
    RUN_TEST(test_withdraw_pushfront_evicts_oldest_waiting);
    RUN_TEST(test_queue_push_pop_preserves_fifo_order);
    RUN_TEST(test_queue_remove_by_seq_keeps_ring_order);

    RUN_TEST(test_store_labels_captured_with_seq_and_label);
    RUN_TEST(test_store_failure_retried_then_succeeds);
    RUN_TEST(test_store_persistent_failure_gives_up);
    RUN_TEST(test_store_failure_does_not_block_next_prompt);
    RUN_TEST(test_label_notification_survives_store_failure);
    RUN_TEST(test_null_store_and_sink_count_drops);
    RUN_TEST(test_retry_buffer_full_gives_up_bounded);

    RUN_TEST(test_late_confirm_after_timeout_still_labels);
    RUN_TEST(test_late_reject_unknown_seq_labels_store);
    RUN_TEST(test_confirm_for_waiting_entry_resolves_and_removes);
    RUN_TEST(test_non_label_commands_not_consumed);
    RUN_TEST(test_button_confirm_with_arg0_zero_targets_offered);
    RUN_TEST(test_button_reject_with_arg0_zero_targets_offered);
    RUN_TEST(test_button_confirm_with_no_offered_prompt_is_noop);

    RUN_TEST(test_provider_false_before_first_step);
    RUN_TEST(test_provider_idle_after_first_step);
    RUN_TEST(test_provider_deferred_then_offered_fields);
    RUN_TEST(test_provider_idle_after_last_resolve);
    RUN_TEST(test_provider_schema_version);

    RUN_TEST(test_stats_zero_initially);
    RUN_TEST(test_stats_mixed_timeline_counts);
    RUN_TEST(test_stats_publish_failure_counted);
    RUN_TEST(test_stats_manager_state_within_ram_budget);

    RUN_TEST(test_ineligible_types_never_queued);
    RUN_TEST(test_own_detector_events_ignored);
    RUN_TEST(test_custom_eligibility_config);
    RUN_TEST(test_default_eligibility_matches_spec);

    RUN_TEST(test_wire_manager_reachable);
    RUN_TEST(test_wire_step_null_clock_waits_honestly);
    RUN_TEST(test_wire_step_reads_alert_status_mailbox);
    RUN_TEST(test_wire_seams_forward_to_manager);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
