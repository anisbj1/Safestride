// bw/alert/alert_engine.cpp - F10 §7 glue: policy ⇒ pattern ⇒ event.

#include <bw/alert/alert_engine.h>

namespace bw::alert {

AlertOutput AlertEngine::step(const AlertInput& input) {
    const PolicyOutput decided = policy_.evaluate(input);

    // §6 D7: the profile that capped the ladder also scales the output, so a
    // Night profile dims the LED of an alert it still allows. profile() is the
    // policy's own view (it holds the newest snapshot), which keeps the
    // ceiling and the intensity on the same data.
    const ModeIntensity profile = policy_.profile();
    player_.setIntensity(profile.led_gain_pct, profile.buzzer_enabled);
    player_.select(patternForLevel(decided.status.level));
    player_.tick(input.t_ms);

    AlertOutput out{};
    out.demo = decided.demo;
    out.wake_check_passed = decided.wake_check_passed;
    out.status = decided.status;
    out.level_changed = decided.level_changed;
    if (decided.level_changed) {
        // One transition ⇒ one event (AlertPolicy's per-step invariant). A
        // missing sink is NotReady, not a silent drop: F10 §3 promises
        // Event{AlertRaised} and a promise must be observable when unmet.
        out.publish_err =
            (events_ == nullptr) ? Err::NotReady : events_->publish(decided.event).err;
    }
    return out;
}

}  // namespace bw::alert
