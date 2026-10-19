#include <bw/hal/buzzer.h>

#if defined(ARDUINO)

#include <Arduino.h>

namespace bw::hal {

namespace {
// [EXPERIMENTAL] Fallback tone for set(true, 0): "on without a frequency"
// must be deterministic and non-silent (and LEDC needs f > 0). 2 kHz sits
// mid-audible where typical passive piezos are loudest; F10 passes real
// tones, this only removes the undefined case. Tune at bench.
constexpr uint16_t kBuzzDefaultFreqHz = 2000;

// [DERIVED] 8-bit duty resolution: 128/256 = 50 % duty is the strongest
// square wave for a passive piezo, and 8 bits keep the arithmetic exact.
constexpr uint8_t kBuzzPwmResolutionBits = 8;
constexpr uint32_t kBuzzPwmDutyHalf = (1u << kBuzzPwmResolutionBits) / 2u;
}  // namespace

void Buzzer::begin() {
    if (passive_) {
        ledcSetup(pwm_channel_, kBuzzDefaultFreqHz, kBuzzPwmResolutionBits);
        ledcAttachPin(static_cast<uint8_t>(pin_), pwm_channel_);
        ledcWrite(pwm_channel_, 0);  // silent at boot
        programmed_freq_hz_ = kBuzzDefaultFreqHz;
        return;
    }
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, active_low_ ? HIGH : LOW);  // off (polarity applied)
}

void Buzzer::set(bool on, uint16_t freq_hz) {
    if (passive_) {
        const uint16_t freq = (freq_hz != 0) ? freq_hz : kBuzzDefaultFreqHz;
        if (freq != programmed_freq_hz_) {
            ledcSetup(pwm_channel_, freq, kBuzzPwmResolutionBits);
            programmed_freq_hz_ = freq;
        }
        ledcWrite(pwm_channel_, on ? kBuzzPwmDutyHalf : 0);
        return;
    }
    // Active buzzer: an on/off level only — freq_hz is ignored by contract
    // (02 §3: "active buzzer ignores freq").
    const bool level_high = active_low_ ? !on : on;
    digitalWrite(pin_, level_high ? HIGH : LOW);
}

}  // namespace bw::hal

#endif  // ARDUINO
