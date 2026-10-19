// Test runner for the test_wave6_dataflow suite (W6 P4, application-level
// cross-feature dataflow — M5 exit evidence, docs/05 step 5). Modeled on
// test_wave4_dataflow; H8 real-neighbor cases land here.
//
// These are real F23/F26 wiring neighbors, not placeholder smoke evidence.
// Other W6 seams remain pending until their explicit cases land.

#include <unity.h>

void test_each_task_has_independent_health_and_supervision(void);
void test_healthy_activity_cannot_mask_stalled_wear_or_wake(void);
void test_healthy_state_cannot_mask_stalled_sleep_or_cataplexy(void);
void test_disabled_research_is_pending_not_a_local_alert_failure(void);
void test_task_creation_failures_reach_real_registry_and_health(void);
void test_event_drop_reaches_real_registry_and_health(void);
void test_acquisition_tee_and_status_reach_diagnostics(void);
void test_watchdog_failure_remains_observable(void);
void test_mode_provider_absence_and_unpublished_keep_original_defaults(void);
void test_mode_commands_relay_to_real_provider_and_state_pump(void);
void test_full_or_absent_mode_relay_does_not_lose_alert_pulses(void);
void test_baseline_absent_and_untrained_do_not_fabricate_hr_evidence(void);
void test_restored_baseline_reaches_actual_f06_injected_estimator(void);
void test_real_held_ble_event_time_confirms_loss_and_recovers(void);
void test_startup_barrier_is_closed_until_composition_publishes(void);
void test_every_device_task_waits_before_touching_the_graph(void);
void test_boot_loop_is_published_without_disabling_local_alerts(void);

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mode_provider_absence_and_unpublished_keep_original_defaults);
    RUN_TEST(test_mode_commands_relay_to_real_provider_and_state_pump);
    RUN_TEST(test_full_or_absent_mode_relay_does_not_lose_alert_pulses);
    RUN_TEST(test_baseline_absent_and_untrained_do_not_fabricate_hr_evidence);
    RUN_TEST(test_restored_baseline_reaches_actual_f06_injected_estimator);
    RUN_TEST(test_real_held_ble_event_time_confirms_loss_and_recovers);
    RUN_TEST(test_startup_barrier_is_closed_until_composition_publishes);
    RUN_TEST(test_every_device_task_waits_before_touching_the_graph);
    RUN_TEST(test_boot_loop_is_published_without_disabling_local_alerts);
    RUN_TEST(test_each_task_has_independent_health_and_supervision);
    RUN_TEST(test_healthy_activity_cannot_mask_stalled_wear_or_wake);
    RUN_TEST(test_healthy_state_cannot_mask_stalled_sleep_or_cataplexy);
    RUN_TEST(test_disabled_research_is_pending_not_a_local_alert_failure);
    RUN_TEST(test_task_creation_failures_reach_real_registry_and_health);
    RUN_TEST(test_event_drop_reaches_real_registry_and_health);
    RUN_TEST(test_acquisition_tee_and_status_reach_diagnostics);
    RUN_TEST(test_watchdog_failure_remains_observable);
    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
