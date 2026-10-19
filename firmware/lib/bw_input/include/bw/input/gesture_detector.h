#pragma once

// bw/input/gesture_detector.h — debounced levels -> gestures (F11 §6 D1/D3/D5).
// Pure, time-driven, no platform calls: every rule is evaluated from the
// caller's timestamps, so a late tick costs latency only — a press held across
// a starvation gap still crosses its threshold on the next tick that runs.
//
// Emission is RELEASE-BASED (D3): Short/Long appear when the button comes up,
// never while it is held, which is what makes the 10 s stuck rule airtight — a
// stuck press can never emit anything.

#include <bw/input/input_constants.h>
#include <bw/input/input_types.h>
#include <bw/types.h>

namespace bw::input {

struct GestureDetectorConfig {
    TimeMs long_press_ms = kLongPressMs;
    TimeMs chord_hold_ms = kChordHoldMs;
    TimeMs chord_release_ms = kChordReleaseMs;
    TimeMs stuck_ms = kButtonStuckMs;
};

class GestureDetector {
public:
    struct Output {
        bool has_event = false;
        GestureRecord event{};
        bool stuck_edge = false;                // one-shot ButtonStuck report for this button
        ButtonId stuck_button = ButtonId::Big;  // valid when stuck_edge
    };

    GestureDetector() = default;
    explicit GestureDetector(const GestureDetectorConfig& cfg) : cfg_(cfg) {}

    // One debounced level sample for one button; the engine feeds BOTH buttons
    // every tick, changed or not (the chord rule needs both current levels).
    // Returns at most one outcome: an event XOR a stuck edge.
    Output step(TimeMs t_ms, ButtonId button, bool pressed, bool changed);

    // Drops all accumulated state (tests; F11 D6 "restart resets state").
    void reset();

private:
    struct ButtonState {
        bool observed = false;       // has this button ever been sampled?
        bool ignored = false;        // boot-held or inside a chord window: silent until release
        bool stuck_latched = false;  // ButtonStuck reported: silent until release
        TimeMs press_start = 0;
    };

    ButtonState& state(ButtonId button);
    const ButtonState& state(ButtonId button) const;
    void abortChord();

    GestureDetectorConfig cfg_{};
    ButtonState states_[2];
    bool level_[2] = {false, false};
    bool chord_active_ = false;     // both down, hold window running
    bool chord_resolving_ = false;  // first release seen, waiting for the partner
    bool chord_ok_ = false;         // hold window was satisfied at first release
    TimeMs both_down_since_ = 0;
    TimeMs suppress_until_ = 0;  // chord-release window (kChordReleaseMs)
};

}  // namespace bw::input
