#pragma once

// bw/alert/pattern_player.h - F10 §6 D2: non-blocking output driving.
//
// The player owns nothing but a pattern id, the current pattern's start time
// and the last values it wrote: `tick(now)` derives the current step from
// elapsed time and writes only on a change, so a 20 Hz tick costs a modulo
// and a comparison (F10 §11 budget). No delay(), no FreeRTOS, no Arduino -
// actuators are the abstract 02 §3 interfaces, so RecordingLed/RecordingBuzzer
// drive the exact same code on the host.
//
// Actuators are optional (nullable): the composition root attaches them when
// it has instances (F03-style deferred DI). A null actuator is a silent,
// dark player, never a crash.

#include <cstdint>

#include <bw/alert/alert_patterns.h>
#include <bw/hardware.h>
#include <bw/types.h>

namespace bw::alert {

class PatternPlayer {
public:
    PatternPlayer() = default;
    PatternPlayer(IRgbLed* led, IBuzzer* buzzer) : led_(led), buzzer_(buzzer) {}

    // Late dependency injection: wire_alert attaches the real actuator (or a
    // test attaches a fake) after construction. Passing nullptr detaches.
    void attach(IRgbLed* led, IBuzzer* buzzer) {
        led_ = led;
        buzzer_ = buzzer;
    }

    // Output profile from OperatingMode (§6 D7). Applied from the next
    // write onwards; an unchanged value costs nothing.
    void setIntensity(uint8_t led_gain_pct, bool buzzer_enabled) {
        gain_pct_ = led_gain_pct;
        buzzer_enabled_ = buzzer_enabled;
    }

    // Selects the pattern to run. A repeat of the current id is a no-op, so
    // ticking at 20 Hz never restarts a pattern that is already playing; a
    // different id restarts it at step 0 on the next tick.
    void select(PatternId id);

    // Advances to `now`. Idempotent: calling it more often only sharpens the
    // timing (a step boundary lands within one tick of its ideal time).
    void tick(TimeMs now);

    PatternId current() const { return id_; }

    // Observed output (tests/diagnostics): what was written last.
    Rgb appliedLed() const { return applied_led_; }
    bool appliedBuzzer() const { return applied_buzzer_on_; }

private:
    // Writes one step's outputs, but only values that changed - unless
    // `force`, which is used once per pattern start so the hardware is put
    // into a known state even if it looks unchanged on paper.
    void apply(const PatternStep& step, bool force);

    IRgbLed* led_ = nullptr;
    IBuzzer* buzzer_ = nullptr;

    PatternId id_ = PatternId::Off;
    Pattern pattern_ = patternFor(PatternId::Off);
    TimeMs start_ms_ = 0;
    bool started_ = false;  // start_ms_ is captured on the first tick
    bool restart_ = true;   // apply step 0 unconditionally on the next tick
    TimeMs cycle_ms_ = 0;   // sum of the step durations (0 = degenerate table)

    uint8_t gain_pct_ = kPercentFull;
    bool buzzer_enabled_ = true;

    Rgb applied_led_{0, 0, 0};
    bool applied_buzzer_on_ = false;
    uint16_t applied_freq_hz_ = 0;
};

}  // namespace bw::alert
