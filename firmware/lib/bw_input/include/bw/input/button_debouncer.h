#pragma once

// bw/input/button_debouncer.h — raw button level -> stable level, per button,
// driven entirely by the caller's timestamps (F11 §5: no blocking debounce, no
// GPIO, no millis(); host-testable). The filter is time-based, so a late tick
// (task starvation) shifts latency, never the accepted pulse width.

#include <bw/input.h>
#include <bw/input/input_constants.h>
#include <bw/input/input_types.h>
#include <bw/types.h>

namespace bw::input {

class ButtonDebouncer {
public:
    struct Level {
        bool pressed;  // stable level after this step
        bool changed;  // true when this step flipped the stable level
    };

    explicit ButtonDebouncer(TimeMs debounce_ms = kDebounceMs);

    // One sample for one button. A level must hold for debounce_ms before it is
    // accepted; a sample earlier than the recorded candidate time (clock
    // rewind) restarts the window instead of accepting instantly.
    Level step(TimeMs t_ms, ButtonId button, bool raw);

    bool pressed(ButtonId button) const;

    // Drops all accumulated state (tests, and F11 D6 "restart resets state").
    void reset();

private:
    struct ButtonState {
        bool candidate = false;  // last raw level seen
        bool stable = false;     // debounced output
        TimeMs candidate_since = 0;
    };

    ButtonState& state(ButtonId button);
    const ButtonState& state(ButtonId button) const;

    TimeMs debounce_ms_;
    ButtonState states_[2];
};

}  // namespace bw::input
