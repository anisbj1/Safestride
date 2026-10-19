#pragma once

// bw/activity/cadence_detector.h — F03 §7: cadence band state machine over
// MotionFeatures.dominant_freq_hz + enmo_g (one call per 1 Hz window).
//
// Pure logic: no platform calls, time is not needed (the classifier owns
// dwell timing). A frequency hovering at an edge cannot toggle the state
// (F03 D4/§8): class → None edges widen by kCadenceHysteresisHz on exit,
// and a class → class edge holds the current state while its own row still
// matches, so the 2.6–2.8 Hz overlap cannot flip the state every window.
// Fresh entries in that overlap are resolved by the ENMO gate with priority
// Running > Walking (F03 D3).

#include <cstdint>

#include <bw/activity/activity_config.h>

namespace bw::activity {

enum class CadenceState : uint8_t {
    None,     // no usable periodicity (dominant_freq_hz == 0, out of band)
    Walking,  // step cadence in the walking band with walking-level motion
    Running,  // cadence ≥ running minimum with running-level impact energy
};

class CadenceDetector {
public:
    CadenceDetector() = default;
    explicit CadenceDetector(const ActivityConfig& config) : config_(config) {}

    // Advances one 1 Hz window and returns the new state (also available
    // via state()). A non-positive dominant frequency means "no periodicity"
    // per the 02 §4.2 contract and is NaN-safe: it forces None. A non-finite
    // enmo_g is treated the same way (garbage input must never enter
    // Running — F03 §10).
    CadenceState step(float dominant_freq_hz, float enmo_g);

    CadenceState state() const { return state_; }

private:
    ActivityConfig config_{};
    CadenceState state_ = CadenceState::None;
};

}  // namespace bw::activity
