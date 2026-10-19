// Test runner for the bw_alert / F10 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-silently never runs.
// (Same pattern as test/bw_activity/test_bw_activity/main.cpp.)

#include <unity.h>
void register_demo_protocol_tests();

// ---- policy: rule table, ladder, windows (test_alert_policy.cpp) ----------
void test_policy_boot_has_no_alert(void);
void test_policy_rules_table(void);
void test_policy_notworn_carries_device_not_worn_reason(void);
void test_policy_raises_warning_with_explainable_event(void);
void test_policy_warning_escalates_after_persistence(void);
void test_policy_active_to_warning_stepdown_is_reachable(void);
void test_policy_warning_level_sustained_escalates_back(void);
void test_policy_demand_change_restarts_the_sustained_timer(void);
void test_policy_deescalation_between_levels_reports_raised(void);
void test_policy_deescalation_holds_the_old_level(void);
void test_policy_resolution_starts_cooldown(void);
void test_policy_ack_stops_and_snoozes_same_level(void);
void test_policy_ack_never_blocks_a_higher_level(void);
void test_policy_ack_with_no_alert_is_a_noop(void);
void test_policy_cancel_starts_cooldown(void);
void test_policy_cancel_held_across_ticks_pushes_cooldown_each_tick(void);
void test_policy_persistent_auto_stops_at_max_duration(void);
void test_policy_persistent_clock_restarts_on_reentry(void);
void test_policy_stale_holds_then_deescalates(void);
void test_policy_stale_recovery_starts_no_cooldown(void);
void test_policy_stale_at_32bit_boundary_time(void);
void test_policy_32bit_duration_fields_saturate(void);
void test_policy_event_floor_raises_and_expires(void);
void test_policy_event_floor_is_gated(void);
void test_policy_event_floor_held_while_gated_fires_after_reopen(void);
void test_policy_other_event_types_do_not_alert(void);
void test_policy_drowsiness_event_floors_warning(void);
void test_policy_mode_ceiling_caps_the_ladder(void);
void test_policy_unknown_mode_falls_back_to_day(void);
void test_policy_status_tracks_since_and_acknowledged(void);
void test_policy_size_within_budget(void);

// ---- patterns (test_pattern_player.cpp) -----------------------------------
void test_patterns_map_levels(void);
void test_player_off_is_dark_and_silent(void);
void test_player_warning_two_beeps_once_then_silent_flashes(void);
void test_player_active_burst_then_rest(void);
void test_player_persistent_is_more_insistent_than_active(void);
void test_player_reselect_does_not_restart(void);
void test_player_intensity_scales_the_led(void);
void test_player_buzzer_disabled_by_profile(void);
void test_player_null_actuators_are_safe(void);
void test_player_time_jump_lands_on_one_step_without_spam(void);
void test_player_backwards_timestamp_is_safe(void);
void test_player_size_within_budget(void);

// ---- engine: glue, events, deferred DI (test_alert_engine.cpp) ------------
void test_engine_raise_publishes_alert_raised(void);
void test_engine_escalation_publishes_a_second_event(void);
void test_engine_ack_publishes_and_flags_the_status(void);
void test_engine_without_sink_reports_not_ready(void);
void test_engine_sink_error_is_not_ignored(void);
void test_engine_night_profile_dims_the_led(void);
void test_engine_pattern_follows_the_level(void);
void test_engine_stale_stops_and_reports(void);
void test_engine_size_within_budget(void);

int main() {
    UNITY_BEGIN();
    register_demo_protocol_tests();

    // --- policy: §6 rule table ---------------------------------------------
    RUN_TEST(test_policy_boot_has_no_alert);
    RUN_TEST(test_policy_rules_table);
    RUN_TEST(test_policy_notworn_carries_device_not_worn_reason);

    // --- policy: ladder timeline -------------------------------------------
    RUN_TEST(test_policy_raises_warning_with_explainable_event);
    RUN_TEST(test_policy_warning_escalates_after_persistence);
    RUN_TEST(test_policy_active_to_warning_stepdown_is_reachable);
    RUN_TEST(test_policy_warning_level_sustained_escalates_back);
    RUN_TEST(test_policy_demand_change_restarts_the_sustained_timer);
    RUN_TEST(test_policy_deescalation_between_levels_reports_raised);
    RUN_TEST(test_policy_deescalation_holds_the_old_level);

    // --- policy: suppression windows ---------------------------------------
    RUN_TEST(test_policy_resolution_starts_cooldown);
    RUN_TEST(test_policy_ack_stops_and_snoozes_same_level);
    RUN_TEST(test_policy_ack_never_blocks_a_higher_level);
    RUN_TEST(test_policy_ack_with_no_alert_is_a_noop);
    RUN_TEST(test_policy_cancel_starts_cooldown);
    RUN_TEST(test_policy_cancel_held_across_ticks_pushes_cooldown_each_tick);
    RUN_TEST(test_policy_persistent_auto_stops_at_max_duration);
    RUN_TEST(test_policy_persistent_clock_restarts_on_reentry);

    // --- policy: failure behaviour (§10) ------------------------------------
    RUN_TEST(test_policy_stale_holds_then_deescalates);
    RUN_TEST(test_policy_stale_recovery_starts_no_cooldown);
    RUN_TEST(test_policy_stale_at_32bit_boundary_time);
    RUN_TEST(test_policy_32bit_duration_fields_saturate);

    // --- policy: event floor (§6 D4) ----------------------------------------
    RUN_TEST(test_policy_event_floor_raises_and_expires);
    RUN_TEST(test_policy_event_floor_is_gated);
    RUN_TEST(test_policy_event_floor_held_while_gated_fires_after_reopen);
    RUN_TEST(test_policy_other_event_types_do_not_alert);
    RUN_TEST(test_policy_drowsiness_event_floors_warning);

    // --- policy: mode profile (§6 D7) ---------------------------------------
    RUN_TEST(test_policy_mode_ceiling_caps_the_ladder);
    RUN_TEST(test_policy_unknown_mode_falls_back_to_day);

    // --- policy: status + budget --------------------------------------------
    RUN_TEST(test_policy_status_tracks_since_and_acknowledged);
    RUN_TEST(test_policy_size_within_budget);

    // --- patterns -----------------------------------------------------------
    RUN_TEST(test_patterns_map_levels);
    RUN_TEST(test_player_off_is_dark_and_silent);
    RUN_TEST(test_player_warning_two_beeps_once_then_silent_flashes);
    RUN_TEST(test_player_active_burst_then_rest);
    RUN_TEST(test_player_persistent_is_more_insistent_than_active);
    RUN_TEST(test_player_reselect_does_not_restart);
    RUN_TEST(test_player_intensity_scales_the_led);
    RUN_TEST(test_player_buzzer_disabled_by_profile);
    RUN_TEST(test_player_null_actuators_are_safe);
    RUN_TEST(test_player_time_jump_lands_on_one_step_without_spam);
    RUN_TEST(test_player_backwards_timestamp_is_safe);
    RUN_TEST(test_player_size_within_budget);

    // --- engine -------------------------------------------------------------
    RUN_TEST(test_engine_raise_publishes_alert_raised);
    RUN_TEST(test_engine_escalation_publishes_a_second_event);
    RUN_TEST(test_engine_ack_publishes_and_flags_the_status);
    RUN_TEST(test_engine_without_sink_reports_not_ready);
    RUN_TEST(test_engine_sink_error_is_not_ignored);
    RUN_TEST(test_engine_night_profile_dims_the_led);
    RUN_TEST(test_engine_pattern_follows_the_level);
    RUN_TEST(test_engine_stale_stops_and_reports);
    RUN_TEST(test_engine_size_within_budget);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
