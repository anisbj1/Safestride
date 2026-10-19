#pragma once

// bw/hal/gpio_buttons.h — the two interaction buttons behind bw::IButtonInput
// (F01). Platform-only file (F01 D5). Polarity is configuration (A-07), not
// code: the driver just maps raw GPIO levels to "logically pressed". Debounce
// and gesture recognition belong to F11 (InputTask) — the HAL exposes the raw
// state only (02 §3: isPressedRaw).

#if defined(ARDUINO)

#include <bw/hardware.h>
#include <bw/input.h>

namespace bw::hal {

class GpioButtons final : public bw::IButtonInput {
public:
    // Pins and polarity come from config/hw_pins.h at wiring time.
    // `active_low` = A-07 (active-low with pull-up, [ASSUMED] — verified at the
    // F01 bench).
    GpioButtons(int big_pin, int small_pin, bool active_low)
        : big_pin_(big_pin), small_pin_(small_pin), active_low_(active_low) {}

    // pinMode setup; called once from wire_hal (IButtonInput has no begin()).
    void begin();

    bool isPressedRaw(bw::ButtonId id) const override;

private:
    int pinFor(bw::ButtonId id) const;

    int big_pin_;
    int small_pin_;
    bool active_low_;
};

}  // namespace bw::hal

#endif  // ARDUINO
