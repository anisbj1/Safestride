# 02 — INTERFACES · hardware-facing interfaces

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 3. Hardware-facing interfaces (implemented by F01 `bw_hal`, faked in `bw_testkit`)

```cpp
enum class SensorStatus : uint8_t { Uninitialized, Ok, Degraded, Failed };

struct ImuSample { TimeMs t_ms; float accel_g[3]; float gyro_dps[3]; };   // sensor frame, no processing
struct PpgSample { TimeMs t_ms; uint32_t red; uint32_t ir; bool saturated; };  // raw ADC counts (18-bit)

class IImuSensor {
 public:
  virtual Result<void> begin() = 0;
  virtual Result<size_t> read(ImuSample* out, size_t max_samples) = 0;   // non-blocking; returns 0 if none new
  virtual SensorStatus status() const = 0;
  virtual Result<void> reinit() = 0;                                     // policy (when) belongs to F26
};
class IPpgSensor { /* same shape; read() drains the FIFO into PpgSample[] with reconstructed timestamps */ };

enum class ButtonId : uint8_t { Big, Small };
class IButtonInput { public: virtual bool isPressedRaw(ButtonId) const = 0; };         // logical pressed, polarity handled in HAL
struct Rgb { uint8_t r, g, b; };
class IRgbLed  { public: virtual void set(Rgb) = 0; };
class IBuzzer  { public: virtual void set(bool on, uint16_t freq_hz) = 0; };           // active buzzer ignores freq
class IDisplayCanvas {                                                                  // 128x64 1-bit; F12 draws, F01 flushes
 public:
  virtual void clear() = 0;
  // (x, y) is the TOP-LEFT of the glyph box — the font's full box, so no glyph
  // can be drawn above y — the same reference drawBitmap uses. (CR-013)
  virtual void drawText(int16_t x, int16_t y, const char* text, FontId font) = 0;
  // (x, y) is the TOP-LEFT of the bitmap.
  virtual void drawBitmap(int16_t x, int16_t y, const uint8_t* xbm, uint8_t w, uint8_t h) = 0;
  virtual void fillRect(int16_t x, int16_t y, uint8_t w, uint8_t h, bool on) = 0;
  virtual void setPower(bool on) = 0;  virtual void setContrast(uint8_t) = 0;
  virtual Result<void> flush() = 0;                                                     // takes the I2cBus lock internally
};
struct BatteryReading { bool valid; float voltage_v; float percent_estimate; bool charging_known; bool charging; };
class IBatteryMonitor { public: virtual BatteryReading read() = 0; };
```
