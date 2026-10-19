#pragma once

// bw/hal/ledc_rgb_led.h — the status RGB LED behind bw::IRgbLed (F01).
// Platform-only file (F01 D5): LEDC PWM via the Arduino-ESP32 2.x API
// (ledcSetup/ledcAttachPin — framework 3.20017 = core 2.0.17). Polarity is a
// configuration flag (A-08: common-anode vs common-cathode unknown until the
// bench); code must work regardless of the answer.

#if defined(ARDUINO)

#include <bw/hardware.h>

namespace bw::hal {

class LedcRgbLed final : public bw::IRgbLed {
public:
    // Pins / LEDC channel base / polarity come from config/hw_pins.h at wiring
    // time. `pwm_channel_base` is the first of three consecutive LEDC channels
    // (ESP32 has 16, 0-15) — R uses base, G base+1, B base+2.
    LedcRgbLed(int r_pin, int g_pin, int b_pin, uint8_t pwm_channel_base, bool active_low)
        : r_pin_(r_pin), g_pin_(g_pin), b_pin_(b_pin), channel_base_(pwm_channel_base),
          active_low_(active_low) {}

    // LEDC timer + channel attach + LED off; called once from wire_hal.
    void begin();

    void set(bw::Rgb rgb) override;

private:
    void writeChannel(uint8_t channel, uint8_t level);

    int r_pin_;
    int g_pin_;
    int b_pin_;
    uint8_t channel_base_;
    bool active_low_;
};

}  // namespace bw::hal

#endif  // ARDUINO
