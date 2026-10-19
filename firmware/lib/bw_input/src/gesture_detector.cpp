#include <bw/input/gesture_detector.h>

namespace bw::input {

namespace {

GestureKey shortKey(ButtonId button) {
    return button == ButtonId::Big ? GestureKey::BigShort : GestureKey::SmallShort;
}

GestureKey longKey(ButtonId button) {
    return button == ButtonId::Big ? GestureKey::BigLong : GestureKey::SmallLong;
}

}  // namespace

GestureDetector::Output GestureDetector::step(TimeMs t_ms, ButtonId button, bool pressed,
                                              bool changed) {
    Output out;
    ButtonState& s = state(button);
    const uint8_t idx = buttonIndex(button);
    const uint8_t other_idx = idx ^ 1u;
    const ButtonId other = other_idx == 0u ? ButtonId::Big : ButtonId::Small;

    // First observation: a button already held at boot is ignored until its
    // first release (F11 §9) — it never becomes a gesture, but its stuck
    // detection still runs below (the hardware truth is not a gesture).
    const bool first_observation = !s.observed;
    s.observed = true;
    if (first_observation && pressed) {
        s.ignored = true;
        s.press_start = t_ms;
    }
    level_[idx] = pressed;

    if (changed && pressed) {
        s.press_start = t_ms;
        if (t_ms < suppress_until_) {
            s.ignored = true;  // rebound inside the chord-release window: silent
        } else if (chord_resolving_) {
            s.ignored = true;  // partner's chord decision still pending: absorb
        } else if (!s.ignored && level_[other_idx] && !state(other).ignored) {
            // Both stable-down now: the chord hold window starts.
            chord_active_ = true;
            chord_ok_ = false;
            both_down_since_ = t_ms;
        }
    }

    if (changed && !pressed) {
        if (s.ignored || s.stuck_latched) {
            // Re-armed on release (D5 / boot-ignore): no gesture. When this
            // happened inside a chord attempt, the attempt is aborted AND the
            // partner button goes silent until its own release — an aborted
            // chord must leave no stray single gesture behind.
            const bool was_chord = chord_active_ || chord_resolving_;
            s.ignored = false;
            s.stuck_latched = false;
            if (was_chord) {
                abortChord();
                if (level_[other_idx]) {
                    state(other).ignored = true;  // partner still down: absorb it too
                }
            }
            return out;
        }

        if (chord_active_ || chord_resolving_) {
            if (chord_active_) {
                // First release of the attempt: silent, decide here whether the
                // simultaneous hold was long enough.
                chord_active_ = false;
                chord_resolving_ = true;
                chord_ok_ =
                    (t_ms >= both_down_since_) && ((t_ms - both_down_since_) >= cfg_.chord_hold_ms);
                return out;
            }
            // Partner's release completes the attempt (D3: the later release
            // emits, so both rebounds are already spent by then).
            chord_resolving_ = false;
            const bool emit = chord_ok_;
            if (emit) {
                out.has_event = true;
                out.event = GestureRecord{t_ms, GestureKey::Chord};
                suppress_until_ = t_ms + cfg_.chord_release_ms;
            }
            return out;  // an aborted attempt emits nothing; singles were absorbed
        }

        // Normal single gesture, release-based (D3).
        const TimeMs held = (t_ms >= s.press_start) ? (t_ms - s.press_start) : 0;
        out.has_event = true;
        out.event =
            GestureRecord{t_ms, held >= cfg_.long_press_ms ? longKey(button) : shortKey(button)};
    }

    // Stuck rule (D5/§10): evaluated every tick while the button is down, so a
    // starved task still reports at the first tick it gets to run.
    if (pressed && !s.stuck_latched && t_ms >= s.press_start &&
        (t_ms - s.press_start) >= cfg_.stuck_ms) {
        s.stuck_latched = true;
        out.stuck_edge = true;
        out.stuck_button = button;
    }

    return out;
}

void GestureDetector::reset() {
    states_[0] = ButtonState{};
    states_[1] = ButtonState{};
    level_[0] = false;
    level_[1] = false;
    chord_active_ = false;
    chord_resolving_ = false;
    chord_ok_ = false;
    both_down_since_ = 0;
    suppress_until_ = 0;
}

void GestureDetector::abortChord() {
    chord_active_ = false;
    chord_resolving_ = false;
    chord_ok_ = false;
}

GestureDetector::ButtonState& GestureDetector::state(ButtonId button) {
    return states_[buttonIndex(button)];
}

const GestureDetector::ButtonState& GestureDetector::state(ButtonId button) const {
    return states_[buttonIndex(button)];
}

}  // namespace bw::input
