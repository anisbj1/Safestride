// Test runner for the bw_ui / F12 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_ml/test_bw_ml/main.cpp.)

#include <unity.h>
void test_demo_fullscreen_centered_blink_and_immediate_clear(void);

// ---- text layout (D7) ------------------------------------------------------
void test_text_layout_caps_match_spec(void);
void test_text_layout_exact_cap_untouched(void);
void test_text_layout_over_cap_truncates(void);
void test_text_layout_empty_string(void);
void test_text_layout_percent_stays_literal(void);
void test_text_layout_format_truncates_nul_safe(void);
void test_text_layout_null_text_is_empty(void);

// ---- screen manager (D3/D4/D6) --------------------------------------------
void test_screen_manager_first_step_is_home_and_powered(void);
void test_screen_manager_empty_command_queue_changes_nothing(void);
void test_screen_manager_cycle_home_diag_home(void);
void test_screen_manager_prompt_skipped_while_unavailable(void);
void test_screen_manager_prompt_enters_cycle_when_available(void);
void test_screen_manager_open_menu_and_back_returns(void);
void test_screen_manager_open_menu_over_diagnostics_back_returns_there(void);
void test_screen_manager_next_screen_moves_menu_cursor(void);
void test_screen_manager_menu_select_stages_semantic_action(void);
void test_screen_manager_menu_ack_stages_acknowledge_alert(void);
void test_screen_manager_non_ui_commands_are_ignored(void);
void test_screen_manager_null_inputs_render_honest_defaults(void);

// ---- prompt screen (D6) ----------------------------------------------------
void test_prompt_screen_placeholder_golden(void);
void test_prompt_screen_availability_flip_only_changes_cycle(void);
void test_prompt_screen_direct_activation_while_unavailable_renders_placeholder(void);
// Backlog 27: the real prompt screen rendering CR-016's ConfirmPromptState.
void test_prompt_screen_offered_renders_all_fields(void);
void test_prompt_screen_offered_golden(void);
void test_prompt_screen_countdown_follows_clock(void);
void test_prompt_screen_deferred_has_no_countdown(void);
void test_prompt_screen_without_clock_renders_unknown_countdown(void);
void test_prompt_screen_no_session_renders_placeholder(void);
void test_prompt_screen_provider_drives_availability_gate(void);
void test_prompt_screen_absent_provider_keeps_w2_default(void);

// ---- home screen (§9) ------------------------------------------------------
void test_home_screen_golden_awake_worn(void);
void test_home_screen_golden_drowsy(void);
void test_home_screen_golden_not_worn(void);
void test_home_screen_golden_unknown_state(void);
void test_home_screen_golden_no_snapshot_yet(void);
void test_home_screen_hr_valid_rounds_to_integer(void);
void test_home_screen_hr_invalid_renders_dashes(void);
void test_home_screen_hr_good_and_degraded_worn_render_number(void);
void test_home_screen_hr_insufficient_quality_renders_dashes(void);
void test_home_screen_hr_requires_fresh_worn_snapshot(void);
void test_home_screen_stale_ppg_renders_dashes(void);
void test_home_screen_battery_absent_renders_dashes(void);
void test_home_screen_forward_timestamp_is_not_stale(void);
void test_home_screen_non_finite_hr_renders_dashes(void);
void test_home_screen_non_finite_battery_renders_dashes(void);

// ---- diagnostics screen (§9) -----------------------------------------------
void test_diagnostics_screen_golden_degraded_sources(void);
void test_diagnostics_screen_golden_no_health_source(void);
void test_diagnostics_screen_empty_health_mailbox_is_na(void);
void test_diagnostics_screen_all_sources_insufficient_shown(void);

// ---- alert overlay (D3/D8) -------------------------------------------------
void test_alert_overlay_warning_banner_over_home(void);
void test_alert_overlay_banner_over_menu(void);
void test_alert_overlay_min_show_persists_after_normal(void);
void test_alert_overlay_rearm_extends_min_show(void);
void test_alert_overlay_persistent_level_keeps_banner(void);
void test_alert_overlay_acknowledged_renders_ack_golden(void);
void test_alert_overlay_warning_and_active_wording(void);
void test_alert_overlay_alert_wakes_timed_out_display(void);
void test_alert_overlay_banner_golden_over_home(void);
void test_alert_overlay_banner_text_fits_offset_cap(void);

// ---- timeout / power (D8) --------------------------------------------------
void test_timeout_power_off_after_inactivity(void);
void test_timeout_command_resets_timer(void);
void test_timeout_alert_raise_wakes_and_resets_timer(void);
void test_timeout_alert_reset_outlives_banner_min_show(void);
void test_timeout_renderer_stops_flushing_when_off(void);
void test_timeout_disabled_without_clock(void);
void test_timeout_zero_disables_sleep(void);

// ---- flush failure (§10) ---------------------------------------------------
void test_flush_failure_counter_rises_and_retries_every_frame(void);
void test_flush_failure_reports_exactly_once_per_episode(void);
void test_flush_failure_recovery_resumes_normal_frames(void);
void test_flush_failure_without_reporter_is_safe(void);
void test_flush_failure_none_while_powered_off(void);

// ---- golden set (§14) ------------------------------------------------------
void test_golden_menu_canonical(void);
void test_golden_menu_cursor_position(void);

// ---- no heap (§9) ----------------------------------------------------------
void test_no_heap_no_allocation_in_steady_state_frames(void);
void test_budget_ui_state_size(void);

// ---- wiring sanity (DoD) ---------------------------------------------------
void test_wire_ui_host_port_accepts_commands(void);
void test_wire_ui_host_wire_binds_context(void);
// Runs after test_wire_ui_host_wire_binds_context on purpose: it asserts the
// bindings that test leaves behind are still live, then re-wires (Low-2).
void test_wire_ui_rewire_binds_second_live_context(void);

// ---- menu closure seam (backlog 32) ----------------------------------------
// Registered AFTER the wiring-sanity block on purpose: they share the file
// statics (s_model, the three queues) with the tests above, so each one
// resets the menu to a known state before asserting.
void test_menu_seam_default_is_closed(void);
void test_menu_seam_select_opens_diagnostics(void);
void test_menu_seam_menu_open_tracks_screen(void);
void test_menu_seam_ack_stages_action(void);
void test_menu_seam_back_restores_previous_screen(void);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_demo_fullscreen_centered_blink_and_immediate_clear);

    RUN_TEST(test_text_layout_caps_match_spec);
    RUN_TEST(test_text_layout_exact_cap_untouched);
    RUN_TEST(test_text_layout_over_cap_truncates);
    RUN_TEST(test_text_layout_empty_string);
    RUN_TEST(test_text_layout_percent_stays_literal);
    RUN_TEST(test_text_layout_format_truncates_nul_safe);
    RUN_TEST(test_text_layout_null_text_is_empty);

    RUN_TEST(test_screen_manager_first_step_is_home_and_powered);
    RUN_TEST(test_screen_manager_empty_command_queue_changes_nothing);
    RUN_TEST(test_screen_manager_cycle_home_diag_home);
    RUN_TEST(test_screen_manager_prompt_skipped_while_unavailable);
    RUN_TEST(test_screen_manager_prompt_enters_cycle_when_available);
    RUN_TEST(test_screen_manager_open_menu_and_back_returns);
    RUN_TEST(test_screen_manager_open_menu_over_diagnostics_back_returns_there);
    RUN_TEST(test_screen_manager_next_screen_moves_menu_cursor);
    RUN_TEST(test_screen_manager_menu_select_stages_semantic_action);
    RUN_TEST(test_screen_manager_menu_ack_stages_acknowledge_alert);
    RUN_TEST(test_screen_manager_non_ui_commands_are_ignored);
    RUN_TEST(test_screen_manager_null_inputs_render_honest_defaults);

    RUN_TEST(test_prompt_screen_placeholder_golden);
    RUN_TEST(test_prompt_screen_availability_flip_only_changes_cycle);
    RUN_TEST(test_prompt_screen_direct_activation_while_unavailable_renders_placeholder);
    RUN_TEST(test_prompt_screen_offered_renders_all_fields);
    RUN_TEST(test_prompt_screen_offered_golden);
    RUN_TEST(test_prompt_screen_countdown_follows_clock);
    RUN_TEST(test_prompt_screen_deferred_has_no_countdown);
    RUN_TEST(test_prompt_screen_without_clock_renders_unknown_countdown);
    RUN_TEST(test_prompt_screen_no_session_renders_placeholder);
    RUN_TEST(test_prompt_screen_provider_drives_availability_gate);
    RUN_TEST(test_prompt_screen_absent_provider_keeps_w2_default);

    RUN_TEST(test_home_screen_golden_awake_worn);
    RUN_TEST(test_home_screen_golden_drowsy);
    RUN_TEST(test_home_screen_golden_not_worn);
    RUN_TEST(test_home_screen_golden_unknown_state);
    RUN_TEST(test_home_screen_golden_no_snapshot_yet);
    RUN_TEST(test_home_screen_hr_valid_rounds_to_integer);
    RUN_TEST(test_home_screen_hr_invalid_renders_dashes);
    RUN_TEST(test_home_screen_hr_good_and_degraded_worn_render_number);
    RUN_TEST(test_home_screen_hr_insufficient_quality_renders_dashes);
    RUN_TEST(test_home_screen_hr_requires_fresh_worn_snapshot);
    RUN_TEST(test_home_screen_stale_ppg_renders_dashes);
    RUN_TEST(test_home_screen_battery_absent_renders_dashes);
    RUN_TEST(test_home_screen_forward_timestamp_is_not_stale);
    RUN_TEST(test_home_screen_non_finite_hr_renders_dashes);
    RUN_TEST(test_home_screen_non_finite_battery_renders_dashes);

    RUN_TEST(test_diagnostics_screen_golden_degraded_sources);
    RUN_TEST(test_diagnostics_screen_golden_no_health_source);
    RUN_TEST(test_diagnostics_screen_empty_health_mailbox_is_na);
    RUN_TEST(test_diagnostics_screen_all_sources_insufficient_shown);

    RUN_TEST(test_alert_overlay_warning_banner_over_home);
    RUN_TEST(test_alert_overlay_banner_over_menu);
    RUN_TEST(test_alert_overlay_min_show_persists_after_normal);
    RUN_TEST(test_alert_overlay_rearm_extends_min_show);
    RUN_TEST(test_alert_overlay_persistent_level_keeps_banner);
    RUN_TEST(test_alert_overlay_acknowledged_renders_ack_golden);
    RUN_TEST(test_alert_overlay_warning_and_active_wording);
    RUN_TEST(test_alert_overlay_alert_wakes_timed_out_display);
    RUN_TEST(test_alert_overlay_banner_golden_over_home);
    RUN_TEST(test_alert_overlay_banner_text_fits_offset_cap);

    RUN_TEST(test_timeout_power_off_after_inactivity);
    RUN_TEST(test_timeout_command_resets_timer);
    RUN_TEST(test_timeout_alert_raise_wakes_and_resets_timer);
    RUN_TEST(test_timeout_alert_reset_outlives_banner_min_show);
    RUN_TEST(test_timeout_renderer_stops_flushing_when_off);
    RUN_TEST(test_timeout_disabled_without_clock);
    RUN_TEST(test_timeout_zero_disables_sleep);

    RUN_TEST(test_flush_failure_counter_rises_and_retries_every_frame);
    RUN_TEST(test_flush_failure_reports_exactly_once_per_episode);
    RUN_TEST(test_flush_failure_recovery_resumes_normal_frames);
    RUN_TEST(test_flush_failure_without_reporter_is_safe);
    RUN_TEST(test_flush_failure_none_while_powered_off);

    RUN_TEST(test_golden_menu_canonical);
    RUN_TEST(test_golden_menu_cursor_position);

    RUN_TEST(test_no_heap_no_allocation_in_steady_state_frames);
    RUN_TEST(test_budget_ui_state_size);

    RUN_TEST(test_wire_ui_host_port_accepts_commands);
    RUN_TEST(test_wire_ui_host_wire_binds_context);
    RUN_TEST(test_wire_ui_rewire_binds_second_live_context);

    RUN_TEST(test_menu_seam_default_is_closed);
    RUN_TEST(test_menu_seam_select_opens_diagnostics);
    RUN_TEST(test_menu_seam_menu_open_tracks_screen);
    RUN_TEST(test_menu_seam_ack_stages_action);
    RUN_TEST(test_menu_seam_back_restores_previous_screen);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
