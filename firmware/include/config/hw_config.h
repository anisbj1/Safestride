#pragma once

// Board-level hardware configuration.                             [F01]
// Numeric values are [EXPERIMENTAL] starting points unless tagged otherwise.
// Pins and polarity live in hw_pins.h; rates, timings and device-type facts
// live here (F01 owns both files).

// Sensor rates (01_ARCHITECTURE.md §3.3)
#define HW_IMU_ODR_HZ 50    // [DERIVED] wrist motion < 20 Hz → Nyquist with margin (§8)
#define HW_PPG_ODR_HZ 25    // [DERIVED] tester: 100 raw sps / 4-sample FIFO averaging

// Task timings
#define HW_SENSOR_PERIOD_MS 20   // SensorTask: IMU 50 Hz = 1000/50
#define HW_PPG_DRAIN_MS 100      // PPG FIFO drained at most every 100 ms (§8: 3× FIFO margin)

// Buzzer type (A-02: passive confirmed by owner 2026-09-29; the HAL still
// supports an active buzzer via this flag — F01 D4, no code change needed).
#define HW_BUZZER_IS_PASSIVE 1  // [HW] A-02

// Battery sense path (BatteryConfig inputs, battery_convert.h).
// A-04: battery has 2 wires only, no divider visible → 0 means every reading
// is reported "unknown" (valid = false) until the F01 bench proves otherwise.
#define HW_BATTERY_DIVIDER_PRESENT 0   // [ASSUMED] A-04 — bench question
// A-06: TP4056 charge-status pins are not wired to the ESP32 → charging state
// is unknown (charging_known = false); set 1 when a future board wires them.
#define HW_BATTERY_CHARGING_KNOWN 0    // [ASSUMED] A-06
#define HW_BATTERY_ADC_FULL_SCALE 4095  // [HW] 12-bit ADC = 2^12 − 1 (esp32-hal-adc)
#define HW_BATTERY_VREF_V 3.3f         // [ASSUMED] dev-board 3.3 V rail; bench-calibrate into HW_BATTERY_CALIBRATION
#define HW_BATTERY_DIVIDER_RATIO 2.0f  // [ASSUMED] (R1+R2)/R2 placeholder until A-04 is answered
#define HW_BATTERY_CALIBRATION 1.0f    // [EXPERIMENTAL] 1.0 until the 3-point multimeter check (§9) measures a factor
#define HW_BATTERY_V_EMPTY_V 3.3f      // [EXPERIMENTAL] linear-percent endpoints for a 3.7 V LiPo; F23/F29 may replace with a curve
#define HW_BATTERY_V_FULL_V 4.2f       // [EXPERIMENTAL] see above
