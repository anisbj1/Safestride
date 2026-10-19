#pragma once

// Pin assignments and polarity facts for the ESP32-WROOM hardware.   [F01]
// Single source of pin/polarity truth (F01 DoD §14): every value carries a
// source comment.
//
// EVERY NUMBER BELOW STARTED [ASSUMED]: the repo has no schematic
// (context/hardware.md §2 — hardware implementation is out of scope) and
// materiel.txt lists no pin numbers. Each was verified — and corrected here —
// at the F01 bench (spec §9 checklist): facts that passed are retagged [HW]
// with their date. Pin choices avoid the ESP32 strapping pins
// (0, 2, 5, 12, 15) and keep battery sensing on ADC1 (ADC2 is unusable
// while BLE is active [HW] — ESP32 errata).

// I²C bus: OLED + MAX30102 + MPU6050 share one bus, arbitrated by I2cBus
// (00 §3). 21/22 are the Arduino-ESP32 core's default Wire pins.
#define HW_PIN_I2C_SDA 21   // [HW] bench 2026-10-01: I²C scan answers 0x3C on these pins
#define HW_PIN_I2C_SCL 22   // [HW] bench 2026-10-01: I²C scan answers 0x3C on these pins

// Display (0.96" SH1106, A-01: controller confirmed) — 7-bit write address.
// Bench row ②/③: scan finds 0x3C every cycle and "F01 alive" renders clean.
#define HW_DISPLAY_ADDR7 0x3C  // [HW] A-01 verified at bench 2026-10-01

// IMU data-ready interrupt (MPU6050 INT). Unused in v1: D2 chooses polling
// at 50 Hz, no FIFO/interrupt consumer exists — reserved for a future
// FIFO-based reader (kept so the wiring fact exists in one place).
#define HW_PIN_IMU_INT 35  // [ASSUMED] input-only pin, no pull needed (push-pull source)

// Buttons (A-07: active-low with internal pull-ups — VERIFIED at bench
// 2026-10-02: both buttons logged clean press/release edges with
// `idle=0, pressed=1` and no spurious flips in 3-minute idle windows).
// 26/27 are general-purpose, support INPUT_PULLUP, not strapping pins.
#define HW_PIN_BUTTON_BIG 26   // [HW] A-07 verified at bench 2026-10-02
#define HW_PIN_BUTTON_SMALL 27 // [HW] A-07 verified at bench 2026-10-02
#define HW_BUTTON_ACTIVE_LOW 1 // [HW] A-07 verified at bench 2026-10-02

// RGB status LED (A-08: polarity unknown — must be configurable, bench
// verify; code works either way). Verified at the F01 bench 2026-10-02:
// R/G/B on 14/13/4 (the developer's final wiring — note red and green are
// the opposite of the original 13/14 guess, corrected here), all output-
// capable, non-strapping, not on the I²C or ADC1 pins.
#define HW_PIN_LED_R 14   // [HW] A-08 verified at bench 2026-10-02
#define HW_PIN_LED_G 13   // [HW] A-08 verified at bench 2026-10-02
#define HW_PIN_LED_B 4    // [HW] A-08 verified at bench 2026-10-02
#define HW_RGB_ACTIVE_LOW 1  // [HW] A-08 confirmed at bench 2026-10-01: the LED is common
                             // anode, so a colour lights when its GPIO is driven LOW. Was 0
                             // [ASSUMED] until the developer measured the part.
// Common-anode feed: the LED's common (anode) pin goes to GPIO25 instead of
// 3V3, so wiring() must drive this pin HIGH before any colour can light.
// One GPIO sources the shared anode current for all 3 colours (a few mA per
// colour, limited by the R/G/B resistors) — well within the ESP32 per-pin
// and per-port sourcing limits.
#define HW_PIN_LED_COMMON 25  // [HW] developer wiring 2026-10-01 (measured at bench)

// Buzzer (A-02: passive confirmed). Bench row ⑥: clear 2 kHz tone on this
// pin, so the pin fact is verified too. Level flag only applies if the board
// ever gets an active buzzer instead (D4 config flag).
#define HW_PIN_BUZZER 16        // [HW] bench 2026-10-01: 2 kHz tone audible on this pin
#define HW_BUZZER_ACTIVE_LOW 0  // [ASSUMED] active-type drive polarity; unused while passive

// Battery voltage sense (A-04: no divider visible on any ADC pin → the pin
// is believed unwired). ADC1 channel — required while BLE runs (ADC2 is
// unusable then [HW]); GPIO34 = ADC1_CH4, input-only (fine for a divider).
#define HW_PIN_BATTERY_ADC 34  // [ASSUMED] A-04 — bench: is any ADC pin wired?

// LEDC channel allocation (ESP32 has 16 channels, 0–15 [HW]). All F01 PWM
// users are assigned here so no two devices can claim a channel:
// RGB = base+0..+2, buzzer = 3; features 4+ start after this file's users.
#define HW_LED_PWM_CHANNEL_BASE 0  // [DERIVED] first of three consecutive channels for R/G/B
#define HW_BUZZER_PWM_CHANNEL 3    // [DERIVED] next free channel after the LED's three
