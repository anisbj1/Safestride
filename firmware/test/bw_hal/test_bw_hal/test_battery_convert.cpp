// Host tests for the pure battery conversion (F01 §9: divider math, A-04
// "no divider -> unknown" behaviour, clamping).

#include <unity.h>

#include <bw/hal/battery_convert.h>

using bw::BatteryReading;
using bw::hal::BatteryConfig;
using bw::hal::batteryFromAdc;

namespace {

// [ASSUMED] Board defaults matching hw_config.h (A-04: divider absent, so
// divider_present flips to true only after the F01 bench confirms it).
BatteryConfig presentConfig() {
    BatteryConfig cfg{};
    cfg.divider_present = true;
    cfg.charging_known = false;  // A-06: TP4056 status pins unwired
    cfg.adc_full_scale = 4095;   // 12-bit ADC
    cfg.vref_v = 3.3f;
    cfg.divider_ratio = 1.5f;
    cfg.calibration = 1.0f;
    cfg.v_empty_v = 3.3f;
    cfg.v_full_v = 4.2f;
    return cfg;
}

}  // namespace

void test_battery_absent_divider_reports_unknown(void) {
    BatteryConfig cfg = presentConfig();
    cfg.divider_present = false;  // A-04: no divider seen on any ADC pin
    const BatteryReading r = batteryFromAdc(2048, cfg);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.voltage_v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.percent_estimate);
    TEST_ASSERT_FALSE(r.charging_known);
    TEST_ASSERT_FALSE(r.charging);
}

void test_battery_counts_convert_to_divider_voltage(void) {
    BatteryConfig cfg = presentConfig();
    // counts/full_scale * vref * ratio = 2048/4095 * 3.3 * 1.5 ≈ 2.475 V
    const BatteryReading r = batteryFromAdc(2048, cfg);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.475f, r.voltage_v);
    TEST_ASSERT_FALSE(r.charging_known);
    TEST_ASSERT_FALSE(r.charging);
}

void test_battery_full_scale_counts_hit_clamped_maximum(void) {
    BatteryConfig cfg = presentConfig();
    // 4095 counts -> 4.95 V, above v_full_v -> percent clamps to 100, not 165.
    const BatteryReading r = batteryFromAdc(4095, cfg);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.95f, r.voltage_v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, r.percent_estimate);
}

void test_battery_zero_counts_clamp_to_empty(void) {
    BatteryConfig cfg = presentConfig();
    const BatteryReading r = batteryFromAdc(0, cfg);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.voltage_v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.percent_estimate);
}

void test_battery_empty_voltage_maps_to_zero_percent(void) {
    BatteryConfig cfg = presentConfig();
    // 3.3 V at the battery = 3.3 / (3.3 * 1.5) * 4095 = 2730 counts.
    const BatteryReading r = batteryFromAdc(2730, cfg);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 3.3f, r.voltage_v);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.percent_estimate);
}

void test_battery_calibration_scales_voltage(void) {
    BatteryConfig cfg = presentConfig();
    cfg.calibration = 1.1f;  // bench-measured correction
    const BatteryReading r = batteryFromAdc(2048, cfg);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.475f * 1.1f, r.voltage_v);
}

void test_battery_zero_full_scale_is_unknown_not_division_by_zero(void) {
    BatteryConfig cfg = presentConfig();
    cfg.adc_full_scale = 0;  // misconfiguration must degrade, not divide by 0
    const BatteryReading r = batteryFromAdc(2048, cfg);
    TEST_ASSERT_FALSE(r.valid);
}
