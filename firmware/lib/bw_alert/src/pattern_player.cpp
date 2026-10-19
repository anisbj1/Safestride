// bw/alert/pattern_player.cpp - F10 §6 D2: time-sliced pattern output.

#include <bw/alert/pattern_player.h>

#include <bw/time.h>

namespace bw::alert {

namespace {

// Scale one channel by the mode's gain (§6 D7) with integer maths only.
// Saturates instead of wrapping if a config ever supplies gain > 100 %.
Rgb scale(Rgb colour, uint8_t gain_pct) {
    const auto channel = [gain_pct](uint8_t value) -> uint8_t {
        const uint16_t scaled =
            static_cast<uint16_t>(static_cast<uint16_t>(value) * gain_pct) / kPercentFull;
        return static_cast<uint8_t>(scaled > kLedFullScale ? kLedFullScale : scaled);
    };
    return Rgb{channel(colour.r), channel(colour.g), channel(colour.b)};
}

}  // namespace

void PatternPlayer::select(PatternId id) {
    if (id == id_) {
        return;  // re-selecting the running pattern must not restart it
    }
    id_ = id;
    pattern_ = patternFor(id);
    cycle_ms_ = 0;
    for (uint8_t i = 0; i < pattern_.count; ++i) {
        cycle_ms_ += pattern_.steps[i].duration_ms;
    }
    started_ = false;  // the new pattern's zero point is the next tick
    restart_ = true;
}

void PatternPlayer::tick(TimeMs now) {
    if (!started_) {
        start_ms_ = now;
        started_ = true;
    }

    // Defensive: patternFor() always returns at least one step, so neither
    // branch is reachable today - they exist so a future table typo degrades
    // to "hold step 0" instead of an unbounded loop inside a real-time task.
    if (pattern_.count == 0) {
        return;
    }
    if (cycle_ms_ == 0) {
        apply(pattern_.steps[0], restart_);
        restart_ = false;
        return;
    }

    // The current step is derived from elapsed time, not accumulated by
    // counting ticks: a delayed or duplicated tick cannot drift the pattern.
    const TimeMs offset = elapsedMs(now, start_ms_) % cycle_ms_;
    TimeMs acc = 0;
    uint8_t index = 0;
    for (; index < pattern_.count; ++index) {
        const TimeMs duration = pattern_.steps[index].duration_ms;
        if (offset < acc + duration) {
            break;
        }
        acc += duration;
    }
    if (index >= pattern_.count) {
        index = static_cast<uint8_t>(pattern_.count - 1);  // exact end of cycle
    }

    apply(pattern_.steps[index], restart_);
    restart_ = false;
}

void PatternPlayer::apply(const PatternStep& step, bool force) {
    const Rgb want_led = scale(step.led, gain_pct_);
    const bool want_on = step.buzz && buzzer_enabled_;
    const uint16_t want_freq = want_on ? step.freq_hz : 0;

    // State is tracked even when no actuator is attached, so a later attach()
    // does not replay a stale value, and so the gain is re-evaluated on every
    // tick (a mode change takes effect within one tick, §6 D7).
    const bool led_changed = want_led.r != applied_led_.r || want_led.g != applied_led_.g ||
                             want_led.b != applied_led_.b;
    const bool buzz_changed =
        want_on != applied_buzzer_on_ || (want_on && want_freq != applied_freq_hz_);

    if (led_ && (force || led_changed)) {
        led_->set(want_led);
    }
    if (buzzer_ && (force || buzz_changed)) {
        buzzer_->set(want_on, want_freq);
    }

    applied_led_ = want_led;
    applied_buzzer_on_ = want_on;
    applied_freq_hz_ = want_freq;
}

}  // namespace bw::alert
