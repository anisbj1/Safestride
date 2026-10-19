#pragma once

// bw/hal/battery_convert.h — ADC counts -> bw::BatteryReading (pure; F01).
// The conversion is separated from analogRead so the divider math, clamping
// and the A-04 "no divider -> unknown" behaviour are host-tested (spec §9).
// No thresholds for decisions here (spec §5): percent is a coarse linear
// estimate, F23 decides anything about low battery.

#include <cstdint>

#include <bw/hardware.h>
#include <bw/types.h>

namespace bw::hal {

// Board-level facts for one battery sense path. Values come from hw_config.h
// at wiring time; unknown facts stay tagged assumptions until the F01 bench.
struct BatteryConfig {
    // [HW/config] false = A-04: no voltage divider seen on any ADC pin ->
    // readings are "unknown" (02 §3: validity flag, never a magic number).
    bool divider_present;
    // [HW/config] A-06: TP4056 charge-status pins are not wired to the ESP32,
    // so charging is unknown. When a future board wires them, set true and
    // feed a real state through the driver.
    bool charging_known;
    // [HW] ADC full-scale counts: 12-bit ESP32 ADC = 2^12 - 1.
    uint16_t adc_full_scale;
    // [ASSUMED] ADC full-scale rail in volts (ESP32 dev-board 3.3 V rail).
    // Bench-calibrate against a multimeter (spec §9) into `calibration`.
    float vref_v;
    // [ASSUMED] Divider ratio (R1+R2)/R2 of the battery divider; placeholder
    // until A-04 is answered at the bench.
    float divider_ratio;
    // [EXPERIMENTAL] Bench correction factor multiplying the ideal divider
    // result (resistor tolerance + ESP32 ADC non-linearity); 1.0 until the
    // 3-point multimeter check supplies a measured value.
    float calibration;
    // [EXPERIMENTAL] Linear percent endpoints for a 3.7 V LiPo: coarse until
    // F23/F29 tune a curve — a curve is the scalable option, not needed yet.
    float v_empty_v;
    float v_full_v;
};

// Counts -> reading. cfg.divider_present == false (or adc_full_scale == 0)
// returns valid = false with zeroed values — "unknown" (A-04). Otherwise:
// voltage = counts / full_scale * vref * divider_ratio * calibration, clamped
// percent is linear between v_empty_v and v_full_v. No allocation, no time.
BatteryReading batteryFromAdc(uint16_t counts, const BatteryConfig& cfg);

}  // namespace bw::hal
