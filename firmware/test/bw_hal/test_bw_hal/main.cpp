// Test runner for the bw_hal suite (F01 §9 host tests).
//
// PlatformIO builds every .cpp in this directory into one program and does NOT
// generate a Unity main, so this file owns it. REGISTER EVERY test function
// here: a function that is never RUN_TEST-ed silently never runs (same
// convention as test/bw_core/test_bw_core/test_main.cpp).
//
// setUp()/tearDown() are strong definitions (weak defaults do not link on this
// MinGW/COFF toolchain — see that file's comment).

#include <unity.h>

// --- I2cBus policy (fake backend) ---
void test_i2cbus_write_success_locks_and_unlocks_once(void);
void test_i2cbus_write_read_passes_rx_buffer(void);
void test_i2cbus_nack_stops_immediately_no_recovery_no_fault(void);
void test_i2cbus_transient_timeout_retries_within_attempts(void);
void test_i2cbus_outage_reports_timeout_recovers_and_succeeds(void);
void test_i2cbus_failed_recovery_reports_stuck(void);
void test_i2cbus_declared_outage_fails_fast_single_attempt(void);
void test_i2cbus_success_rearms_so_next_outage_reports_again(void);
void test_i2cbus_lock_timeout_returns_busy_without_touching_the_bus(void);
void test_i2cbus_null_clock_and_reporter_still_returns_errors(void);
void test_i2cbus_scan_collects_acknowledging_addresses(void);
void test_i2cbus_scan_aborts_on_bus_error_after_one_outage(void);
void test_i2cbus_scan_rejects_missing_arguments(void);
// --- SensorStatusTracker ---
void test_sensor_status_fresh_tracker_is_uninitialized(void);
void test_sensor_status_begin_ok_marks_ok(void);
void test_sensor_status_absent_at_boot_is_failed_immediately(void);
void test_sensor_status_failures_below_threshold_stay_ok(void);
void test_sensor_status_nth_consecutive_failure_transitions_once(void);
void test_sensor_status_success_resets_the_streak(void);
void test_sensor_status_reinit_after_failure_restores_ok(void);
void test_sensor_status_read_ok_after_absent_covers_replug(void);
void test_sensor_status_zero_failures_means_fail_on_first(void);

// --- MAX30102 FIFO parser ---
void test_ppg_parser_empty_input_yields_zero_samples(void);
void test_ppg_parser_decodes_msb_first_sample(void);
void test_ppg_parser_masks_18_bit_and_flags_full_scale(void);
void test_ppg_parser_ir_full_scale_alone_saturates(void);
void test_ppg_parser_reconstructs_timestamps_backwards(void);
void test_ppg_parser_timestamps_clamp_at_boot(void);
void test_ppg_parser_cap_keeps_absolute_timestamps(void);
void test_ppg_parser_rejects_invalid_inputs(void);
void test_ppg_fifo_pointer_wrap_around(void);
void test_ppg_fifo_pointer_masks_upper_bits(void);

// --- MPU6050 conversion ---
void test_be16_reads_msb_first(void);
void test_be16_sign_extends_negative_values(void);
void test_accel_lsb_per_g_matches_datasheet_table(void);
void test_gyro_lsb_per_dps_matches_datasheet_table(void);
void test_convert_zero_raw_is_zero_and_keeps_timestamp(void);
void test_convert_configured_range_one_g(void);
void test_convert_covers_every_range_extremes(void);
void test_convert_int16_extremes_do_not_overflow(void);
void test_convert_full_burst_fixture_pipeline(void);

// --- Mpu6050Imu driver (scripted register device) ---
void test_mpu_begin_with_valid_id_configures_and_marks_ok(void);
void test_mpu_begin_wrong_whoami_reports_absent_once(void);
void test_mpu_begin_nack_reports_absent(void);
void test_mpu_begin_bus_error_propagates_and_stays_uninitialized(void);
void test_mpu_read_returns_sample_with_timestamp(void);
void test_mpu_five_consecutive_failures_report_read_failed_once(void);
void test_mpu_success_between_failures_resets_streak(void);
void test_mpu_read_after_replug_recovers_without_reinit(void);
void test_mpu_read_zero_max_samples_does_not_touch_the_bus(void);

void test_mpu_diag_returns_raw_counts_and_register_readbacks(void);

void test_mpu_diag_failure_leaves_streak_and_faults_untouched(void);

void test_mpu_diag_success_does_not_reset_failure_streak(void);

void test_mpu_diag_absent_returns_not_found_without_reporting(void);

// --- Max30102Ppg driver (scripted register device) ---
void test_ppg_begin_writes_config_and_marks_ok(void);
void test_ppg_begin_wrong_part_id_reports_absent_once(void);
void test_ppg_begin_nack_reports_absent(void);
void test_ppg_boot_leftover_overrun_recovers_to_ok(void);
void test_ppg_begin_soft_resets_and_waits_before_configuring(void);
void test_ppg_begin_stuck_reset_fails_bounded(void);
void test_ppg_read_empty_fifo_returns_zero_without_fifo_traffic(void);
void test_ppg_read_drains_samples_with_reconstructed_timestamps(void);
void test_ppg_read_caps_but_keeps_absolute_timestamps(void);
void test_ppg_fifo_overrun_reports_once_per_episode(void);
void test_ppg_overrun_episode_does_not_latch_failed(void);
void test_ppg_persistent_overrun_reaches_failed_and_recovers(void);
void test_ppg_begin_clone_ignoring_writes_recovers_boot_overrun(void);
void test_ppg_runtime_stuck_ovf_adapts_and_keeps_data_flowing(void);
void test_ppg_read_nack_failures_report_read_failed_once(void);
void test_ppg_read_zero_max_samples_does_not_touch_the_bus(void);

// --- Wire error mapping (pure) ---
void test_errmap_success_is_ok(void);
void test_errmap_addr_nack_is_not_found(void);
void test_errmap_timeout_is_timeout(void);
void test_errmap_data_nack_too_long_and_other_are_bus_error(void);
void test_errmap_unknown_code_is_bus_error(void);
void test_read_failure_probe_ack_means_timeout(void);
void test_read_failure_probe_nack_means_not_found(void);
void test_read_failure_probe_timeout_and_other_map_through(void);

// --- Battery conversion (pure) ---
void test_battery_absent_divider_reports_unknown(void);
void test_battery_counts_convert_to_divider_voltage(void);
void test_battery_full_scale_counts_hit_clamped_maximum(void);
void test_battery_zero_counts_clamp_to_empty(void);
void test_battery_empty_voltage_maps_to_zero_percent(void);
void test_battery_calibration_scales_voltage(void);
void test_battery_zero_full_scale_is_unknown_not_division_by_zero(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_i2cbus_write_success_locks_and_unlocks_once);
    RUN_TEST(test_i2cbus_write_read_passes_rx_buffer);
    RUN_TEST(test_i2cbus_nack_stops_immediately_no_recovery_no_fault);
    RUN_TEST(test_i2cbus_transient_timeout_retries_within_attempts);
    RUN_TEST(test_i2cbus_outage_reports_timeout_recovers_and_succeeds);
    RUN_TEST(test_i2cbus_failed_recovery_reports_stuck);
    RUN_TEST(test_i2cbus_declared_outage_fails_fast_single_attempt);
    RUN_TEST(test_i2cbus_success_rearms_so_next_outage_reports_again);
    RUN_TEST(test_i2cbus_lock_timeout_returns_busy_without_touching_the_bus);
    RUN_TEST(test_i2cbus_null_clock_and_reporter_still_returns_errors);
    RUN_TEST(test_i2cbus_scan_collects_acknowledging_addresses);
    RUN_TEST(test_i2cbus_scan_aborts_on_bus_error_after_one_outage);
    RUN_TEST(test_i2cbus_scan_rejects_missing_arguments);

    RUN_TEST(test_sensor_status_fresh_tracker_is_uninitialized);
    RUN_TEST(test_sensor_status_begin_ok_marks_ok);
    RUN_TEST(test_sensor_status_absent_at_boot_is_failed_immediately);
    RUN_TEST(test_sensor_status_failures_below_threshold_stay_ok);
    RUN_TEST(test_sensor_status_nth_consecutive_failure_transitions_once);
    RUN_TEST(test_sensor_status_success_resets_the_streak);
    RUN_TEST(test_sensor_status_reinit_after_failure_restores_ok);
    RUN_TEST(test_sensor_status_read_ok_after_absent_covers_replug);
    RUN_TEST(test_sensor_status_zero_failures_means_fail_on_first);

    RUN_TEST(test_ppg_parser_empty_input_yields_zero_samples);
    RUN_TEST(test_ppg_parser_decodes_msb_first_sample);
    RUN_TEST(test_ppg_parser_masks_18_bit_and_flags_full_scale);
    RUN_TEST(test_ppg_parser_ir_full_scale_alone_saturates);
    RUN_TEST(test_ppg_parser_reconstructs_timestamps_backwards);
    RUN_TEST(test_ppg_parser_timestamps_clamp_at_boot);
    RUN_TEST(test_ppg_parser_cap_keeps_absolute_timestamps);
    RUN_TEST(test_ppg_parser_rejects_invalid_inputs);
    RUN_TEST(test_ppg_fifo_pointer_wrap_around);
    RUN_TEST(test_ppg_fifo_pointer_masks_upper_bits);

    RUN_TEST(test_be16_reads_msb_first);
    RUN_TEST(test_be16_sign_extends_negative_values);
    RUN_TEST(test_accel_lsb_per_g_matches_datasheet_table);
    RUN_TEST(test_gyro_lsb_per_dps_matches_datasheet_table);
    RUN_TEST(test_convert_zero_raw_is_zero_and_keeps_timestamp);
    RUN_TEST(test_convert_configured_range_one_g);
    RUN_TEST(test_convert_covers_every_range_extremes);
    RUN_TEST(test_convert_int16_extremes_do_not_overflow);
    RUN_TEST(test_convert_full_burst_fixture_pipeline);

    RUN_TEST(test_mpu_begin_with_valid_id_configures_and_marks_ok);
    RUN_TEST(test_mpu_begin_wrong_whoami_reports_absent_once);
    RUN_TEST(test_mpu_begin_nack_reports_absent);
    RUN_TEST(test_mpu_begin_bus_error_propagates_and_stays_uninitialized);
    RUN_TEST(test_mpu_read_returns_sample_with_timestamp);
    RUN_TEST(test_mpu_five_consecutive_failures_report_read_failed_once);
    RUN_TEST(test_mpu_success_between_failures_resets_streak);
    RUN_TEST(test_mpu_read_after_replug_recovers_without_reinit);
    RUN_TEST(test_mpu_read_zero_max_samples_does_not_touch_the_bus);
    RUN_TEST(test_mpu_diag_returns_raw_counts_and_register_readbacks);
    RUN_TEST(test_mpu_diag_failure_leaves_streak_and_faults_untouched);
    RUN_TEST(test_mpu_diag_success_does_not_reset_failure_streak);
    RUN_TEST(test_mpu_diag_absent_returns_not_found_without_reporting);

    RUN_TEST(test_ppg_begin_writes_config_and_marks_ok);
    RUN_TEST(test_ppg_begin_wrong_part_id_reports_absent_once);
    RUN_TEST(test_ppg_begin_nack_reports_absent);
    RUN_TEST(test_ppg_boot_leftover_overrun_recovers_to_ok);
    RUN_TEST(test_ppg_begin_soft_resets_and_waits_before_configuring);
    RUN_TEST(test_ppg_begin_stuck_reset_fails_bounded);
    RUN_TEST(test_ppg_read_empty_fifo_returns_zero_without_fifo_traffic);
    RUN_TEST(test_ppg_read_drains_samples_with_reconstructed_timestamps);
    RUN_TEST(test_ppg_read_caps_but_keeps_absolute_timestamps);
    RUN_TEST(test_ppg_fifo_overrun_reports_once_per_episode);
    RUN_TEST(test_ppg_overrun_episode_does_not_latch_failed);
    RUN_TEST(test_ppg_persistent_overrun_reaches_failed_and_recovers);
    RUN_TEST(test_ppg_begin_clone_ignoring_writes_recovers_boot_overrun);
    RUN_TEST(test_ppg_runtime_stuck_ovf_adapts_and_keeps_data_flowing);
    RUN_TEST(test_ppg_read_nack_failures_report_read_failed_once);
    RUN_TEST(test_ppg_read_zero_max_samples_does_not_touch_the_bus);

    RUN_TEST(test_errmap_success_is_ok);
    RUN_TEST(test_errmap_addr_nack_is_not_found);
    RUN_TEST(test_errmap_timeout_is_timeout);
    RUN_TEST(test_errmap_data_nack_too_long_and_other_are_bus_error);
    RUN_TEST(test_errmap_unknown_code_is_bus_error);
    RUN_TEST(test_read_failure_probe_ack_means_timeout);
    RUN_TEST(test_read_failure_probe_nack_means_not_found);
    RUN_TEST(test_read_failure_probe_timeout_and_other_map_through);

    RUN_TEST(test_battery_absent_divider_reports_unknown);
    RUN_TEST(test_battery_counts_convert_to_divider_voltage);
    RUN_TEST(test_battery_full_scale_counts_hit_clamped_maximum);
    RUN_TEST(test_battery_zero_counts_clamp_to_empty);
    RUN_TEST(test_battery_empty_voltage_maps_to_zero_percent);
    RUN_TEST(test_battery_calibration_scales_voltage);
    RUN_TEST(test_battery_zero_full_scale_is_unknown_not_division_by_zero);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
