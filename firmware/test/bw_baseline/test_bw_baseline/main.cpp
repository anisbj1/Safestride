// Test runner for the bw_baseline / F13 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_cataplexy/test_bw_cataplexy/main.cpp.)

#include <unity.h>

// ---- tracker learning (§9: 1, 2, 3, 14) ------------------------------------
void test_profile_absent_before_any_data(void);
void test_learning_converges_on_qualified_stream(void);
void test_adaptation_tracks_drift(void);
void test_confidence_ramp(void);

// ---- gates (§9: 4, 5, 6, 7, 9) ---------------------------------------------
void test_outlier_rejection_and_band_floor(void);
void test_not_worn_windows_excluded(void);
void test_poor_quality_windows_excluded(void);
void test_activity_gate_and_calibration_widening(void);
void test_hr_plausibility_clamp(void);
void test_nonfinite_motion_cannot_poison_baseline(void);
void test_pump_counts_only_new_sensor_pairs(void);
void test_pump_rejects_stale_context(void);
void test_pump_hold_without_evidence_arms_cooldown(void);

// ---- poisoning guard (§9: 8) ------------------------------------------------
void test_poisoning_hold_and_cooldown(void);

// ---- serializer (§9: 10, 11, 12) --------------------------------------------
void test_serializer_round_trip(void);
void test_serializer_rejects_corrupt(void);
void test_serializer_truncated_or_oversized(void);

// ---- restart / budgets / time (§9: 13, 16, 17) -----------------------------
void test_warm_restart_keeps_state(void);
void test_state_sizes_fit_budget(void);
void test_time_gap_and_reboot_boundary(void);

// ---- wiring seams (§9: 15, 18 + three additive checks) ---------------------
void test_adapter_publish_semantics(void);
void test_store_absent_boots_in_memory(void);
void test_wiring_constants_match_spec(void);
void test_wire_corrupt_store_reports_baseline_fault(void);
void test_wire_restores_valid_blob(void);
void test_pump_persist_cadence_and_failed_write_retry(void);
void test_pump_persists_valid_transition_before_next_interval(void);

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_profile_absent_before_any_data);
    RUN_TEST(test_learning_converges_on_qualified_stream);
    RUN_TEST(test_adaptation_tracks_drift);
    RUN_TEST(test_confidence_ramp);

    RUN_TEST(test_outlier_rejection_and_band_floor);
    RUN_TEST(test_not_worn_windows_excluded);
    RUN_TEST(test_poor_quality_windows_excluded);
    RUN_TEST(test_activity_gate_and_calibration_widening);
    RUN_TEST(test_hr_plausibility_clamp);
    RUN_TEST(test_nonfinite_motion_cannot_poison_baseline);
    RUN_TEST(test_pump_counts_only_new_sensor_pairs);
    RUN_TEST(test_pump_rejects_stale_context);
    RUN_TEST(test_pump_hold_without_evidence_arms_cooldown);

    RUN_TEST(test_poisoning_hold_and_cooldown);

    RUN_TEST(test_serializer_round_trip);
    RUN_TEST(test_serializer_rejects_corrupt);
    RUN_TEST(test_serializer_truncated_or_oversized);

    RUN_TEST(test_warm_restart_keeps_state);
    RUN_TEST(test_state_sizes_fit_budget);
    RUN_TEST(test_time_gap_and_reboot_boundary);

    RUN_TEST(test_adapter_publish_semantics);
    RUN_TEST(test_store_absent_boots_in_memory);
    RUN_TEST(test_wiring_constants_match_spec);
    RUN_TEST(test_wire_corrupt_store_reports_baseline_fault);
    RUN_TEST(test_wire_restores_valid_blob);
    RUN_TEST(test_pump_persist_cadence_and_failed_write_retry);
    RUN_TEST(test_pump_persists_valid_transition_before_next_interval);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
