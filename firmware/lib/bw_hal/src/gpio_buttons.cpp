#include <bw/hal/gpio_buttons.h>

#if defined(ARDUINO)

#include <Arduino.h>

namespace bw::hal {

void GpioButtons::begin() {
    // A-07 [ASSUMED] active-low with pull-up: the internal pull-up (~45 kΩ)
    // is plenty for a tactile switch to VCC, so INPUT_PULLUP encodes both the
    // wiring and the polarity in one place. If the bench finds active-high
    // wiring, `active_low` flips in hw_pins.h and this line selects plain
    // INPUT (an external resistor must then hold the level).
    pinMode(big_pin_, active_low_ ? INPUT_PULLUP : INPUT);
    pinMode(small_pin_, active_low_ ? INPUT_PULLUP : INPUT);
}

int GpioButtons::pinFor(bw::ButtonId id) const {
    return id == bw::ButtonId::Big ? big_pin_ : small_pin_;
}

bool GpioButtons::isPressedRaw(bw::ButtonId id) const {
    const bool raw_high = digitalRead(pinFor(id)) == HIGH;
    // No debounce here: F11's InputTask owns debounce/gestures (HAL design);
    // the HAL exposes raw logical state only (02 §3: isPressedRaw).
    return active_low_ ? !raw_high : raw_high;
}

}  // namespace bw::hal

#endif  // ARDUINO
