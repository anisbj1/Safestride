#include <bw/input/button_debouncer.h>

namespace bw::input {

ButtonDebouncer::ButtonDebouncer(TimeMs debounce_ms) : debounce_ms_(debounce_ms) {}

ButtonDebouncer::Level ButtonDebouncer::step(TimeMs t_ms, ButtonId button, bool raw) {
    ButtonState& s = state(button);

    // New level, or a clock that went backwards: (re)start the stability
    // window so a rewind can never satisfy it with a huge unsigned difference.
    if (raw != s.candidate || t_ms < s.candidate_since) {
        s.candidate = raw;
        s.candidate_since = t_ms;
    }

    if (s.candidate != s.stable && (t_ms - s.candidate_since) >= debounce_ms_) {
        s.stable = s.candidate;
        return Level{s.stable, true};
    }
    return Level{s.stable, false};
}

bool ButtonDebouncer::pressed(ButtonId button) const {
    return state(button).stable;
}

void ButtonDebouncer::reset() {
    states_[0] = ButtonState{};
    states_[1] = ButtonState{};
}

ButtonDebouncer::ButtonState& ButtonDebouncer::state(ButtonId button) {
    return states_[buttonIndex(button)];
}

const ButtonDebouncer::ButtonState& ButtonDebouncer::state(ButtonId button) const {
    return states_[buttonIndex(button)];
}

}  // namespace bw::input
