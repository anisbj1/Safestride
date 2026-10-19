#include <bw/hal/ledc_rgb_led.h>

#if defined(ARDUINO)

#include <Arduino.h>

namespace bw::hal {

namespace {
// [DERIVED] LED PWM carrier: 5 kHz is 250× above flicker fusion (no visible
// flicker at any duty), a value the LEDC timer realizes exactly at 8-bit
// resolution (Arduino-ESP32's own ledc example pair is 5000 Hz / 8 bit), and
// above the audible band's trouble spots so the driver stage cannot whine.
constexpr uint32_t kLedPwmFreqHz = 5000;

// [DERIVED] Duty resolution: matches Rgb's 8-bit channels with no truncation
// (256 levels in → 256 levels out); 8 bits are also within what the LEDC
// timer can produce at kLedPwmFreqHz.
constexpr uint8_t kLedPwmResolutionBits = 8;
constexpr uint32_t kLedPwmMaxDuty = (1u << kLedPwmResolutionBits) - 1u;
}  // namespace

void LedcRgbLed::begin() {
    ledcSetup(channel_base_, kLedPwmFreqHz, kLedPwmResolutionBits);
    ledcSetup(static_cast<uint8_t>(channel_base_ + 1), kLedPwmFreqHz, kLedPwmResolutionBits);
    ledcSetup(static_cast<uint8_t>(channel_base_ + 2), kLedPwmFreqHz, kLedPwmResolutionBits);
    ledcAttachPin(static_cast<uint8_t>(r_pin_), channel_base_);
    ledcAttachPin(static_cast<uint8_t>(g_pin_), static_cast<uint8_t>(channel_base_ + 1));
    ledcAttachPin(static_cast<uint8_t>(b_pin_), static_cast<uint8_t>(channel_base_ + 2));
    // Off at boot for either polarity (A-08: polarity unknown until bench —
    // the off state must be unambiguous in both configurations).
    set(bw::Rgb{0, 0, 0});
}

void LedcRgbLed::writeChannel(uint8_t channel, uint8_t level) {
    // Common-anode (active low): duty is inverted — level 255 = pin HIGH =
    // LED off; common-cathode passes the level through.
    const uint32_t duty = active_low_ ? (kLedPwmMaxDuty - level) : level;
    ledcWrite(channel, duty);
}

void LedcRgbLed::set(bw::Rgb rgb) {
    writeChannel(channel_base_, rgb.r);
    writeChannel(static_cast<uint8_t>(channel_base_ + 1), rgb.g);
    writeChannel(static_cast<uint8_t>(channel_base_ + 2), rgb.b);
}

}  // namespace bw::hal

#endif  // ARDUINO
