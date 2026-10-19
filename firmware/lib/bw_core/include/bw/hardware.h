#pragma once

// bw/hardware.h — docs/02_INTERFACES.md §3 (hardware-facing samples + interfaces)
// and §7's FontId.
//
// F00 declares these; F01 (bw_hal) implements them, bw_testkit fakes them.
// No behavior lives here. Interfaces have no virtual destructor on purpose (contract
// shape): instances are owned by the composition root and never deleted through a
// base pointer (DEC-002: no RTTI/exceptions).

#include <cstddef>
#include <cstdint>

#include <bw/input.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw {

enum class SensorStatus : uint8_t { Uninitialized, Ok, Degraded, Failed };

// One IMU reading in the sensor frame, before any processing (02 §3).
struct ImuSample {
    TimeMs t_ms;
    float accel_g[3];
    float gyro_dps[3];
};

// Raw PPG ADC counts (18-bit, red + IR; no green — MH-ET Live MAX30102, 00 §3).
struct PpgSample {
    TimeMs t_ms;
    uint32_t red;
    uint32_t ir;
    bool saturated;
};

class IImuSensor {
public:
    virtual Result<void> begin() = 0;
    // Non-blocking; returns 0 samples if none are new (02 §3).
    virtual Result<size_t> read(ImuSample* out, size_t max_samples) = 0;
    virtual SensorStatus status() const = 0;
    virtual Result<void> reinit() = 0;  // policy (when to reinit) belongs to F26
};

class IPpgSensor {
public:
    virtual Result<void> begin() = 0;
    // Drains the FIFO into PpgSample[] with reconstructed timestamps; non-blocking,
    // returns 0 samples if none are new (02 §3).
    virtual Result<size_t> read(PpgSample* out, size_t max_samples) = 0;
    virtual SensorStatus status() const = 0;
    virtual Result<void> reinit() = 0;
};

class IButtonInput {
public:
    // Logical pressed state; polarity (active-low, pull-up) is handled in the HAL
    // (A-07: assumed active-low with pull-up, verified at F01 bench).
    virtual bool isPressedRaw(ButtonId) const = 0;
};

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

class IRgbLed {
public:
    virtual void set(Rgb) = 0;
};

class IBuzzer {
public:
    // Active buzzers ignore freq_hz (A-02: passive confirmed, HAL still supports
    // active via config); freq is the tone for passive buzzers.
    virtual void set(bool on, uint16_t freq_hz) = 0;
};

// Exactly the three values 02 §7 names for F00 to define — nothing more.
enum class FontId : uint8_t { Small, Medium, Large };

// 128x64 1-bit canvas; F12 draws, F01 flushes onto the SH1106 over I2C (02 §3).
// flush() takes the I2cBus lock internally (single bus owner: F01, 00 §3).
class IDisplayCanvas {
public:
    virtual void clear() = 0;
    // (x, y) is the TOP-LEFT of the glyph box — the font's full box, so no glyph
    // can be drawn above y — the same reference drawBitmap uses. (CR-013)
    virtual void drawText(int16_t x, int16_t y, const char* text, FontId font) = 0;
    // (x, y) is the TOP-LEFT of the bitmap.
    virtual void drawBitmap(int16_t x, int16_t y, const uint8_t* xbm, uint8_t w, uint8_t h) = 0;
    virtual void fillRect(int16_t x, int16_t y, uint8_t w, uint8_t h, bool on) = 0;
    virtual void setPower(bool on) = 0;
    virtual void setContrast(uint8_t) = 0;
    virtual Result<void> flush() = 0;
};

// `valid == false` (not a magic voltage) when the divider is absent — A-04: no
// voltage divider seen; battery is reported "unknown" until proven otherwise.
struct BatteryReading {
    bool valid;
    float voltage_v;
    float percent_estimate;
    bool charging_known;
    bool charging;
};

class IBatteryMonitor {
public:
    virtual BatteryReading read() = 0;
};

}  // namespace bw
