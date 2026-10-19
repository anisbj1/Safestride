// Test runner for the bw_signal / F02 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_core/test_bw_core/test_main.cpp — one strong
// setUp/tearDown definition per suite, weak defaults do not link on this
// toolchain.)

#include <unity.h>

void test_tester_hr_placement_average_expiry_and_recontact();
void test_tester_hr_captured_waveform_and_noise();
void test_tester_profile_ambient_noise_is_insufficient_without_contact();
void test_tester_hr_four_value_rollover_and_saturation();
void test_tester_profile_windowed_pipeline_captured_waveform();

// ---- modules ---------------------------------------------------------------
void test_gravity_seeds_from_first_sample(void);
void test_gravity_tilt_six_orientations(void);
void test_gravity_converges_on_slow_change(void);

void test_scheduler_boundary_close(void);
void test_scheduler_stall_close_at_end_plus_grace(void);
void test_scheduler_advance_clears_boundary(void);

void test_imu_constant_one_g_zero_enmo(void);
void test_imu_metrics_for_known_motion(void);
void test_imu_stillness_enter_exit_hysteresis(void);
void test_imu_motion_does_not_enter_stillness(void);
void test_imu_gap_marks_degraded_and_resets_stillness(void);
void test_imu_no_samples_insufficient(void);
void test_imu_partial_frame_below_coverage_insufficient(void);
void test_imu_backwards_timestamp_marks_anomaly(void);
void test_imu_dominant_frequency_from_sine(void);
void test_imu_dominant_frequency_zero_when_constant(void);

void test_filter_constant_dc_settles_immediately(void);
void test_filter_removes_dc_and_keeps_1hz(void);
void test_filter_corner_gain_half_at_0_5hz(void);
void test_filter_attenuates_out_of_band(void);
void test_filter_debug_ring_is_rolling(void);

void test_beat_detector_counts_beats_on_sine(void);
void test_beat_detector_refractory_rejects_double(void);
void test_beat_detector_floor_rejects_small_peaks(void);

void test_hr_median_from_regular_beats(void);
void test_hr_invalid_when_too_few_beats(void);
void test_hr_invalid_when_timing_inconsistent(void);
void test_hr_rejects_impossible_median(void);

void test_quality_clean_window_good(void);
void test_quality_no_samples_insufficient(void);
void test_quality_low_coverage_insufficient(void);
void test_quality_saturation_insufficient(void);
void test_quality_low_perfusion_insufficient(void);
void test_quality_gap_degraded(void);
void test_quality_anomaly_degraded(void);
void test_quality_poor_timing_degrades_sqi(void);

// ---- integration -----------------------------------------------------------
void test_processor_hr_synthetic_70bpm(void);
void test_processor_hr_synthetic_50bpm(void);
void test_processor_hr_synthetic_120bpm(void);
void test_processor_hr_realdata_golden(void);
void test_processor_beat_starvation_recovers(void);
void test_processor_noise_above_floor_never_reports_good_quality(void);
void test_processor_noise_only_insufficient(void);
void test_processor_saturated_insufficient(void);
void test_processor_hr_valid_when_quality_insufficient(void);
void test_processor_hr_survives_amplitude_jitter(void);
void test_processor_hr_valid_holds_through_weak_dip(void);
void test_processor_hr_clears_within_gap_when_pulse_stops(void);
void test_processor_hr_first_display_latency(void);
void test_processor_arrhythmia_hold_does_not_rescue(void);
void test_estimator_missed_beats_tolerated_arrhythmia_rejected(void);
void test_processor_ppg_window_diagnostics(void);
void test_processor_motion_corrupted_flagged(void);
void test_processor_no_data_stall_insufficient(void);
void test_processor_step_until_idle_catches_up_delayed_empty_windows(void);
void test_processor_boundary_close_with_carry(void);
void test_processor_backlog_catchup_closes_all_windows(void);
void test_processor_backwards_sample_marks_degraded(void);
void test_processor_debug_tap_returns_points(void);
void test_processor_size_within_budget(void);

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_tester_hr_placement_average_expiry_and_recontact);
    RUN_TEST(test_tester_hr_captured_waveform_and_noise);
    RUN_TEST(test_tester_profile_ambient_noise_is_insufficient_without_contact);
    RUN_TEST(test_tester_hr_four_value_rollover_and_saturation);
    RUN_TEST(test_tester_profile_windowed_pipeline_captured_waveform);

    RUN_TEST(test_gravity_seeds_from_first_sample);
    RUN_TEST(test_gravity_tilt_six_orientations);
    RUN_TEST(test_gravity_converges_on_slow_change);

    RUN_TEST(test_scheduler_boundary_close);
    RUN_TEST(test_scheduler_stall_close_at_end_plus_grace);
    RUN_TEST(test_scheduler_advance_clears_boundary);

    RUN_TEST(test_imu_constant_one_g_zero_enmo);
    RUN_TEST(test_imu_metrics_for_known_motion);
    RUN_TEST(test_imu_stillness_enter_exit_hysteresis);
    RUN_TEST(test_imu_motion_does_not_enter_stillness);
    RUN_TEST(test_imu_gap_marks_degraded_and_resets_stillness);
    RUN_TEST(test_imu_no_samples_insufficient);
    RUN_TEST(test_imu_partial_frame_below_coverage_insufficient);
    RUN_TEST(test_imu_backwards_timestamp_marks_anomaly);
    RUN_TEST(test_imu_dominant_frequency_from_sine);
    RUN_TEST(test_imu_dominant_frequency_zero_when_constant);

    RUN_TEST(test_filter_constant_dc_settles_immediately);
    RUN_TEST(test_filter_removes_dc_and_keeps_1hz);
    RUN_TEST(test_filter_corner_gain_half_at_0_5hz);
    RUN_TEST(test_filter_attenuates_out_of_band);
    RUN_TEST(test_filter_debug_ring_is_rolling);

    RUN_TEST(test_beat_detector_counts_beats_on_sine);
    RUN_TEST(test_beat_detector_refractory_rejects_double);
    RUN_TEST(test_beat_detector_floor_rejects_small_peaks);

    RUN_TEST(test_hr_median_from_regular_beats);
    RUN_TEST(test_hr_invalid_when_too_few_beats);
    RUN_TEST(test_hr_invalid_when_timing_inconsistent);
    RUN_TEST(test_hr_rejects_impossible_median);

    RUN_TEST(test_quality_clean_window_good);
    RUN_TEST(test_quality_no_samples_insufficient);
    RUN_TEST(test_quality_low_coverage_insufficient);
    RUN_TEST(test_quality_saturation_insufficient);
    RUN_TEST(test_quality_low_perfusion_insufficient);
    RUN_TEST(test_quality_gap_degraded);
    RUN_TEST(test_quality_anomaly_degraded);
    RUN_TEST(test_quality_poor_timing_degrades_sqi);

    RUN_TEST(test_processor_hr_synthetic_70bpm);
    RUN_TEST(test_processor_hr_synthetic_50bpm);
    RUN_TEST(test_processor_hr_synthetic_120bpm);
    RUN_TEST(test_processor_hr_realdata_golden);
    RUN_TEST(test_processor_beat_starvation_recovers);
    RUN_TEST(test_processor_noise_above_floor_never_reports_good_quality);
    RUN_TEST(test_processor_noise_only_insufficient);
    RUN_TEST(test_processor_saturated_insufficient);
    RUN_TEST(test_processor_hr_valid_when_quality_insufficient);
    RUN_TEST(test_processor_hr_survives_amplitude_jitter);
    RUN_TEST(test_processor_hr_valid_holds_through_weak_dip);
    RUN_TEST(test_processor_hr_clears_within_gap_when_pulse_stops);
    RUN_TEST(test_processor_hr_first_display_latency);
    RUN_TEST(test_processor_arrhythmia_hold_does_not_rescue);
    RUN_TEST(test_estimator_missed_beats_tolerated_arrhythmia_rejected);
    RUN_TEST(test_processor_ppg_window_diagnostics);
    RUN_TEST(test_processor_motion_corrupted_flagged);
    RUN_TEST(test_processor_no_data_stall_insufficient);
    RUN_TEST(test_processor_step_until_idle_catches_up_delayed_empty_windows);
    RUN_TEST(test_processor_boundary_close_with_carry);
    RUN_TEST(test_processor_backlog_catchup_closes_all_windows);
    RUN_TEST(test_processor_backwards_sample_marks_degraded);
    RUN_TEST(test_processor_debug_tap_returns_points);
    RUN_TEST(test_processor_size_within_budget);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
