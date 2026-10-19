#pragma once

// bw/hal/buzzer.h — the audible alert transducer behind bw::IBuzzer (F01).
// Platform-only file (F01 D5). A-02: the board's buzzer is passive
// (confirmed), so tones are LEDC PWM at freq_hz; the `passive` flag keeps the
// active-buzzer variant working without code changes (hardware-agnostic
// configuration, F01 D4).

#if defined(ARDUINO)

#include <bw/hardware.h>

namespace bw::hal {

class Buzzer final : public bw::IBuzzer {
public:
    // Pin / type / polarity / LEDC channel come from config/hw_pins.h and
    // config/hw_config.h at wiring time. `active_low` applies to the active
    // type only (a PWM waveform has no polarity).
    Buzzer(int pin, bool passive, bool active_low, uint8_t pwm_channel)
        : pin_(pin), passive_(passive), active_low_(active_low), pwm_channel_(pwm_channel) {}

    // GPIO or LEDC setup + silent; called once from wire_hal.
    void begin();

    void set(bool on, uint16_t freq_hz) override;

private:
    int pin_;
    bool passive_;
    bool active_low_;
    uint8_t pwm_channel_;
    uint16_t programmed_freq_hz_ = 0;  // last PWM tone; avoids re-setup churn
};

}  // namespace bw::hal

#endif  // ARDUINO
