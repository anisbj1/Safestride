// Test runner for the bw_modes / F24 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_ble/test_bw_ble/main.cpp.)

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// ---- transition matrix (spec 9 cases 1-6, 7.1, 6 D3) -----------------------
void test_matrix_setmode_all_25_pairs(void);
void test_self_transition_is_noop(void);
void test_enter_research_stores_and_restores(void);
void test_exit_research_only_legal_in_research(void);
void test_start_nap_semantics(void);
void test_setmode_out_of_enum_rejected(void);
void test_setmode_to_research_rejected_context_preserved(void);
void test_foreign_command_rejected(void);

// ---- auto schedule (spec 9 cases 7-12, 6 D4) -------------------------------
void test_schedule_flips_to_night_at_220000(void);
void test_schedule_day_at_070000_and_midnight_wrap(void);
void test_schedule_never_fires_without_wall_time(void);
void test_schedule_suppressed_outside_day_night(void);
void test_explicit_mode_survives_until_next_crossing(void);
void test_boot_inside_night_window_initial_sync(void);
void test_command_and_crossing_same_tick_reports_net_effect(void);

// ---- power policy (spec 9 cases 13-16, 6 D6) -------------------------------
void test_power_enter_exit_boundaries(void);
void test_power_invalid_battery_forces_normal(void);
void test_power_charging_overrides_low_battery(void);
void test_profile_values_for_all_mode_power_combos(void);
void test_power_nonfinite_percent_forces_normal(void);

// ---- transition observation (spec 9 cases 17-19; G1 ruling 1: no events) --
void test_one_changed_flag_per_transition_with_reason(void);
void test_no_changed_on_noop_reject_or_power_only(void);
void test_rejected_command_not_retried_and_state_not_stuck(void);

// ---- wiring compile + pump seam (spec 7.2, DoD 14) -------------------------
void test_wire_getters_honest_before_wire(void);
void test_wire_idempotent_init_and_first_pump(void);
void test_wire_command_sink_set_mode_via_pump(void);
void test_wire_queue_full_refused_and_bounded_drain(void);
void test_wire_battery_drives_power_axis(void);
void test_wire_wall_time_known_activates_auto(void);
void test_wire_provider_interface_matches_mailbox(void);

// Order note: the wiring tests share one f24:: static state and must run in
// the registered order (test_wire_modes_compile.cpp header).
int main(int, char**) {
    UNITY_BEGIN();

    // transition matrix
    RUN_TEST(test_matrix_setmode_all_25_pairs);
    RUN_TEST(test_self_transition_is_noop);
    RUN_TEST(test_enter_research_stores_and_restores);
    RUN_TEST(test_exit_research_only_legal_in_research);
    RUN_TEST(test_start_nap_semantics);
    RUN_TEST(test_setmode_out_of_enum_rejected);
    RUN_TEST(test_setmode_to_research_rejected_context_preserved);
    RUN_TEST(test_foreign_command_rejected);

    // auto schedule
    RUN_TEST(test_schedule_flips_to_night_at_220000);
    RUN_TEST(test_schedule_day_at_070000_and_midnight_wrap);
    RUN_TEST(test_schedule_never_fires_without_wall_time);
    RUN_TEST(test_schedule_suppressed_outside_day_night);
    RUN_TEST(test_explicit_mode_survives_until_next_crossing);
    RUN_TEST(test_boot_inside_night_window_initial_sync);
    RUN_TEST(test_command_and_crossing_same_tick_reports_net_effect);

    // power policy
    RUN_TEST(test_power_enter_exit_boundaries);
    RUN_TEST(test_power_invalid_battery_forces_normal);
    RUN_TEST(test_power_charging_overrides_low_battery);
    RUN_TEST(test_profile_values_for_all_mode_power_combos);
    RUN_TEST(test_power_nonfinite_percent_forces_normal);

    // transition observation (no events in W6, G1 ruling 1)
    RUN_TEST(test_one_changed_flag_per_transition_with_reason);
    RUN_TEST(test_no_changed_on_noop_reject_or_power_only);
    RUN_TEST(test_rejected_command_not_retried_and_state_not_stuck);

    // wiring (LAST: shared static state, order-dependent)
    RUN_TEST(test_wire_getters_honest_before_wire);
    RUN_TEST(test_wire_idempotent_init_and_first_pump);
    RUN_TEST(test_wire_command_sink_set_mode_via_pump);
    RUN_TEST(test_wire_queue_full_refused_and_bounded_drain);
    RUN_TEST(test_wire_battery_drives_power_axis);
    RUN_TEST(test_wire_wall_time_known_activates_auto);
    RUN_TEST(test_wire_provider_interface_matches_mailbox);

    return UNITY_END();
}
