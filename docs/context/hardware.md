# 00 — CONTEXT · scope and hardware (§2–3)

> Part of `00_PROJECT_CONTEXT.md`; locked.

## 2. Scope of the current phase

In scope: **software architecture, firmware, signal processing, ML, data handling, communication, companion application**.
Out of scope: hardware implementation (wiring, PCB, enclosure) — handled separately. The hardware is a breadboard prototype; every hardware variant is configuration. Firmware still needs hardware facts; they live in one place (`hw_pins.h`, owned by F01) and unknown facts are listed as assumptions (§3).

The system is designed as a complete intelligent wearable, not a collection of sensor demos.

---

## 3. Hardware platform

| Component | Role | Firmware-relevant notes |
|---|---|---|
| ESP32-WROOM | Primary MCU | Dual-core 240 MHz, ~520 KB SRAM, **flash assumed 4 MB [ASSUMED]**, BLE used, classic BT unused, **no battery-backed RTC** (wall-clock time must come from the phone) |
| ESP32-CAM | Secondary/experimental vision module | Separate MCU + separate firmware; optional; AI-Thinker variants have a microSD slot [ASSUMED]. Never required for core function |
| 0.96" OLED | Local UI | **SH1106 128×64 over I²C @ 0x3C [controller CONFIRMED; address ASSUMED]** |
| MH-ET Live MAX30102 | PPG / HR | I²C 0x57; red + IR LEDs only (no green); FIFO 32 samples; SpO₂ is out of scope |
| GY-87 | IMU | **MPU6050** (I²C 0x68): accel + gyro — the only part required. Also carries a magnetometer (HMC5883L 0x1E or clone QMC5883L 0x0D — verify) and a barometer (BMP180/BMP280 0x77), unused in v1. The magnetometer sits behind the MPU6050 bypass/aux bus |
| TP4056 | Li-Po charging | Charge-status pins may or may not be wired to the ESP32 [ASSUMED unknown] |
| 3.7 V 3000 mAh Li-Po | Power | Battery-voltage ADC divider **[ASSUMED present]**; if absent, battery level is reported "unknown" |
| 5 V buzzer | Audible alerts | **Active vs. passive unknown [ASSUMED unknown]** → HAL supports both through configuration |
| RGB LED | Status/alert | Common-anode vs. common-cathode unknown → HAL configuration |
| Big button / small button | Interaction | Active-low with pull-up [ASSUMED] |

**Shared I²C bus.** OLED, MAX30102 and MPU6050 (± mag/baro) share one bus. Access must be arbitrated by one owner (`I2cBus` in F01) with a mutex, timeouts and bus-recovery logic. No other module touches I²C.

---
