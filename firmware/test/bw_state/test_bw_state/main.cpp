// Test runner for the bw_state / F07 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_wear/test_bw_wear/main.cpp.)

#include <unity.h>
void test_snapshot_motion_evidence_keeps_original_timestamp_and_gates(void);
void test_demo_accepted_response_recovers_all_probabilistic_rungs_not_gates();

// ---- smoother (spec §9, D2/D7) --------------------------------------------
void test_smoother_first_sample_seeds(void);
void test_smoother_alpha_applied_per_step(void);
void test_smoother_reset_reseeds(void);
void test_smoother_nan_propagates_why_d13_validates_upstream(void);

// ---- duration counter (spec §9, D6/D8) ------------------------------------
void test_counter_accumulates_while_active(void);
void test_counter_first_active_instant_is_zero(void);
void test_counter_resets_never_pauses(void);
void test_counter_sustained_inclusive_at_boundary(void);
void test_counter_projection_reads_between_updates(void);
void test_counter_saturates_at_max_duration(void);
void test_counter_future_and_backward_now_never_underflow(void);
void test_counter_reset_clears_run(void);

// ---- gates (spec §7 table G1/G2/G3a, §9, §10) -----------------------------
void test_gates_boot_no_input_is_unknown_sensor_unavailable(void);
void test_gates_uncertain_wear_cannot_advance_to_awake();
void test_gates_notworn_beats_insufficient_quality(void);
void test_gates_stale_wear_cannot_claim_not_worn(void);
void test_gates_stale_boundary_is_exactly_input_stale_ms(void);
void test_gates_valid_false_gates(void);
void test_gates_alertness_stale_boundary(void);
void test_gates_single_insufficient_source_does_not_gate(void);
void test_gates_both_sources_insufficient_gates(void);
void test_gates_alertness_insufficient_quality_gates(void);
void test_gates_g3a_unknown_first_then_sustained_calm(void);
void test_gates_unknown_recovers_one_second_after_gates_clear(void);
void test_gates_quality_blip_while_unknown_resets_recovery_dwell(void);
void test_gates_flapping_notworn_worn_follows_gate(void);
void test_gates_flapping_worn_uncertain_bounded_transitions(void);

// ---- ladder E1..E4 (spec §7 table, §9, D8) --------------------------------
void test_ladder_drowsy_enters_at_dwell_not_before(void);
void test_ladder_below_enter_never_enters(void);
void test_ladder_enter_boundary_counts(void);
void test_ladder_dip_resets_dwell(void);
void test_ladder_high_risk_after_second_dwell(void);
void test_ladder_mid_band_stays_drowsy(void);
void test_ladder_drowsy_alone_never_possible_sleep(void);
void test_ladder_possible_sleep_after_third_dwell(void);
void test_ladder_e2_deescalates_immediately(void);
void test_ladder_e2_possible_sleep_to_high_risk(void);
void test_ladder_e2_sleep_070_holds_possible_sleep(void);
void test_ladder_e3_calm_boundary(void);
void test_ladder_short_motion_marker_seeds_at_stillness_onset(void);
void test_ladder_ordinary_estimate_keeps_ema_smoothing(void);
void test_ladder_active_motion_recovers_from_drowsy_within_five_seconds(void);
void test_ladder_hysteresis_band_holds_drowsy(void);
void test_ladder_e4_ambiguous_stays_unknown(void);
void test_ladder_e1_from_gate_pays_full_dwell(void);
void test_ladder_flapping_noise_bounded_transitions(void);
void test_ladder_monotonic_in_sleep_probability(void);

// ---- snapshot / confidence / reasons (spec §9, D9/D10/D13/D14) ------------
void test_snapshot_unknown_zero_confidence(void);
void test_snapshot_notworn_confidence_is_wear(void);
void test_snapshot_ladder_confidence_is_product(void);
void test_snapshot_degraded_source_penalizes_and_explains(void);
void test_snapshot_two_degraded_sources_squared(void);
void test_snapshot_confidence_clamped_to_one(void);
void test_snapshot_d14_reasons_pass_through_on_ladder(void);
void test_snapshot_d14_no_pass_through_on_unknown(void);
void test_snapshot_d14_no_pass_through_on_rejected_input(void);
void test_snapshot_invalid_probabilities_gate(void);
void test_snapshot_mirrors_axes_and_stale_axes_collapse(void);
void test_snapshot_in_state_ms_accumulates_and_resets(void);
void test_snapshot_state_provider(void);
void test_snapshot_sizes_within_budget(void);

// ---- transition records (spec §9, D11/D12) --------------------------------
void test_events_drowsiness_on_drowsy_escalation(void);
void test_events_high_risk_payload_prev_and_new(void);
void test_events_single_record_per_escalation(void);
void test_events_no_record_on_deescalation_recovery_or_unknown(void);
void test_events_no_record_for_possible_sleep(void);
void test_events_devicenotworn_record(void);
void test_events_record_copies_transition_snapshot(void);
void test_events_registry_and_reserved_fields(void);

// ---- 1 Hz pump (spec §9, D4/D11/D15; auditor A-03) ------------------------
void test_pump_1hz_drives_engine_from_mailboxes(void);
void test_pump_stalled_features_age_to_insufficient(void);
void test_pump_records_published_or_dropped(void);
void test_pump_wake_check_sequence_timestamp_and_drowsy_gate(void);
void test_pump_wake_check_rejects_invalid_timestamps_and_non_drowsy(void);
void test_pump_wake_check_does_not_override_notworn_or_unknown(void);
void test_pump_short_motion_marker_is_one_tick_and_starts_dwell_at_onset(void);

// ---- P4b quality provider (spec §9/D16, Wave 4; H10) -----------------------
void test_pump_provider_insufficient_gates_while_features_good(void);
void test_pump_provider_good_beats_stalled_features(void);
void test_pump_provider_silence_ages_then_gates(void);
void test_pump_provider_never_received_insufficient_despite_healthy_features(void);

// ---- P4b FIX loop 1 (audit repair: recovery + mid-rung + defensive seed) ---
void test_pump_provider_good_recovers_after_gate_full_dwell(void);
void test_pump_provider_gate_mid_rung_requires_full_rung_dwell(void);
void test_pump_provider_true_without_write_is_insufficient(void);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_snapshot_motion_evidence_keeps_original_timestamp_and_gates);
    RUN_TEST(test_demo_accepted_response_recovers_all_probabilistic_rungs_not_gates);

    RUN_TEST(test_smoother_first_sample_seeds);
    RUN_TEST(test_smoother_alpha_applied_per_step);
    RUN_TEST(test_smoother_reset_reseeds);
    RUN_TEST(test_smoother_nan_propagates_why_d13_validates_upstream);

    RUN_TEST(test_counter_accumulates_while_active);
    RUN_TEST(test_counter_first_active_instant_is_zero);
    RUN_TEST(test_counter_resets_never_pauses);
    RUN_TEST(test_counter_sustained_inclusive_at_boundary);
    RUN_TEST(test_counter_projection_reads_between_updates);
    RUN_TEST(test_counter_saturates_at_max_duration);
    RUN_TEST(test_counter_future_and_backward_now_never_underflow);
    RUN_TEST(test_counter_reset_clears_run);

    RUN_TEST(test_gates_boot_no_input_is_unknown_sensor_unavailable);
    RUN_TEST(test_gates_uncertain_wear_cannot_advance_to_awake);
    RUN_TEST(test_gates_notworn_beats_insufficient_quality);
    RUN_TEST(test_gates_stale_wear_cannot_claim_not_worn);
    RUN_TEST(test_gates_stale_boundary_is_exactly_input_stale_ms);
    RUN_TEST(test_gates_valid_false_gates);
    RUN_TEST(test_gates_alertness_stale_boundary);
    RUN_TEST(test_gates_single_insufficient_source_does_not_gate);
    RUN_TEST(test_gates_both_sources_insufficient_gates);
    RUN_TEST(test_gates_alertness_insufficient_quality_gates);
    RUN_TEST(test_gates_g3a_unknown_first_then_sustained_calm);
    RUN_TEST(test_gates_unknown_recovers_one_second_after_gates_clear);
    RUN_TEST(test_gates_quality_blip_while_unknown_resets_recovery_dwell);
    RUN_TEST(test_gates_flapping_notworn_worn_follows_gate);
    RUN_TEST(test_gates_flapping_worn_uncertain_bounded_transitions);

    RUN_TEST(test_ladder_drowsy_enters_at_dwell_not_before);
    RUN_TEST(test_ladder_below_enter_never_enters);
    RUN_TEST(test_ladder_enter_boundary_counts);
    RUN_TEST(test_ladder_dip_resets_dwell);
    RUN_TEST(test_ladder_high_risk_after_second_dwell);
    RUN_TEST(test_ladder_mid_band_stays_drowsy);
    RUN_TEST(test_ladder_drowsy_alone_never_possible_sleep);
    RUN_TEST(test_ladder_possible_sleep_after_third_dwell);
    RUN_TEST(test_ladder_e2_deescalates_immediately);
    RUN_TEST(test_ladder_e2_possible_sleep_to_high_risk);
    RUN_TEST(test_ladder_e2_sleep_070_holds_possible_sleep);
    RUN_TEST(test_ladder_e3_calm_boundary);
    RUN_TEST(test_ladder_short_motion_marker_seeds_at_stillness_onset);
    RUN_TEST(test_ladder_ordinary_estimate_keeps_ema_smoothing);
    RUN_TEST(test_ladder_active_motion_recovers_from_drowsy_within_five_seconds);
    RUN_TEST(test_ladder_hysteresis_band_holds_drowsy);
    RUN_TEST(test_ladder_e4_ambiguous_stays_unknown);
    RUN_TEST(test_ladder_e1_from_gate_pays_full_dwell);
    RUN_TEST(test_ladder_flapping_noise_bounded_transitions);
    RUN_TEST(test_ladder_monotonic_in_sleep_probability);

    RUN_TEST(test_snapshot_unknown_zero_confidence);
    RUN_TEST(test_snapshot_notworn_confidence_is_wear);
    RUN_TEST(test_snapshot_ladder_confidence_is_product);
    RUN_TEST(test_snapshot_degraded_source_penalizes_and_explains);
    RUN_TEST(test_snapshot_two_degraded_sources_squared);
    RUN_TEST(test_snapshot_confidence_clamped_to_one);
    RUN_TEST(test_snapshot_d14_reasons_pass_through_on_ladder);
    RUN_TEST(test_snapshot_d14_no_pass_through_on_unknown);
    RUN_TEST(test_snapshot_d14_no_pass_through_on_rejected_input);
    RUN_TEST(test_snapshot_invalid_probabilities_gate);
    RUN_TEST(test_snapshot_mirrors_axes_and_stale_axes_collapse);
    RUN_TEST(test_snapshot_in_state_ms_accumulates_and_resets);
    RUN_TEST(test_snapshot_state_provider);
    RUN_TEST(test_snapshot_sizes_within_budget);

    RUN_TEST(test_events_drowsiness_on_drowsy_escalation);
    RUN_TEST(test_events_high_risk_payload_prev_and_new);
    RUN_TEST(test_events_single_record_per_escalation);
    RUN_TEST(test_events_no_record_on_deescalation_recovery_or_unknown);
    RUN_TEST(test_events_no_record_for_possible_sleep);
    RUN_TEST(test_events_devicenotworn_record);
    RUN_TEST(test_events_record_copies_transition_snapshot);
    RUN_TEST(test_events_registry_and_reserved_fields);

    RUN_TEST(test_pump_1hz_drives_engine_from_mailboxes);
    RUN_TEST(test_pump_stalled_features_age_to_insufficient);
    RUN_TEST(test_pump_records_published_or_dropped);
    RUN_TEST(test_pump_wake_check_sequence_timestamp_and_drowsy_gate);
    RUN_TEST(test_pump_wake_check_rejects_invalid_timestamps_and_non_drowsy);
    RUN_TEST(test_pump_wake_check_does_not_override_notworn_or_unknown);
    RUN_TEST(test_pump_short_motion_marker_is_one_tick_and_starts_dwell_at_onset);

    RUN_TEST(test_pump_provider_insufficient_gates_while_features_good);
    RUN_TEST(test_pump_provider_good_beats_stalled_features);
    RUN_TEST(test_pump_provider_silence_ages_then_gates);
    RUN_TEST(test_pump_provider_never_received_insufficient_despite_healthy_features);
    RUN_TEST(test_pump_provider_good_recovers_after_gate_full_dwell);
    RUN_TEST(test_pump_provider_gate_mid_rung_requires_full_rung_dwell);
    RUN_TEST(test_pump_provider_true_without_write_is_insufficient);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
