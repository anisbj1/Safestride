#include <cmath>

#include <bw/activity/cadence_detector.h>

namespace bw::activity {

namespace {

// True while the frequency is still inside the state's own (unwidened)
// cadence band — i.e. its row of the F03 §6 decision table still matches.
// Energy is deliberately not checked here: the exit check in step() only
// lets a state survive while enmo_g meets that state's enter floor.
bool rowStillMatches(CadenceState state, float freq_hz, const ActivityConfig& c) {
    switch (state) {
    case CadenceState::Running:
        return freq_hz >= c.running_cadence_min_hz && freq_hz <= c.running_cadence_max_hz;
    case CadenceState::Walking:
        return freq_hz >= c.walking_cadence_min_hz && freq_hz <= c.walking_cadence_max_hz;
    case CadenceState::None:
        break;
    }
    return false;  // None has no row to hold: a fresh entry always enters.
}

}  // namespace

CadenceState CadenceDetector::step(float dominant_freq_hz, float enmo_g) {
    // Contract (02 §4.2): 0 when there is no dominant frequency. Treat every
    // non-positive value — NaN included — as "no cadence evidence". Energy
    // must be finite too: a NaN/±Inf ENMO is garbage input (spec §10), so it
    // is handled exactly like a non-finite frequency — force None, never
    // Running (ENMO is the gate that decides the 2.6–2.8 Hz overlap, D3).
    if (!(dominant_freq_hz > 0.0f) || !std::isfinite(enmo_g)) {
        state_ = CadenceState::None;
        return state_;
    }

    const ActivityConfig& c = config_;

    // Exit first, with the band widened by cadence_hysteresis_hz so a
    // hovering frequency keeps its state (F03 D4). Energy is checked with
    // the same enter floor: a quiet window is not walking/running at all.
    CadenceState next = state_;
    switch (state_) {
    case CadenceState::Running:
        if (dominant_freq_hz < c.running_cadence_min_hz - c.cadence_hysteresis_hz ||
            dominant_freq_hz > c.running_cadence_max_hz + c.cadence_hysteresis_hz ||
            enmo_g < c.running_enmo_min_g) {
            next = CadenceState::None;
        }
        break;
    case CadenceState::Walking:
        if (dominant_freq_hz < c.walking_cadence_min_hz - c.cadence_hysteresis_hz ||
            dominant_freq_hz > c.walking_cadence_max_hz + c.cadence_hysteresis_hz ||
            enmo_g < c.walking_enmo_min_g) {
            next = CadenceState::None;
        }
        break;
    case CadenceState::None:
        break;
    }

    // Hold second (F03 D4 / §8): if the exit check kept the state and its own
    // row still matches, skip the enter step — the current state wins. This
    // is the class-to-class hysteresis: without it the enter priority alone
    // (Running > Walking, D3) would flip CadenceState every window while a
    // frequency hovers in the 2.6–2.8 Hz overlap. Holding turns that overlap
    // into a Schmitt band — Walking → Running only above walking_max_hz,
    // Running → Walking only below running_min_hz. That class-to-class band
    // is the gap walking_max_hz − running_min_hz (0.2 Hz here, the same
    // number as kCadenceHysteresisHz today but derived from the band edges,
    // so tuning either one alone changes the width) — so a frequency hovering
    // at the class-to-class edge cannot toggle the state. D3's priority still
    // resolves every fresh entry (state None) and every edge the current row
    // has left (e.g. 2.9 Hz above walking_max_hz ⇒ Running).
    const bool hold = next != CadenceState::None && rowStillMatches(state_, dominant_freq_hz, c);

    if (!hold) {
        // Enter: priority Running > Walking resolves the 2.6–2.8 Hz overlap
        // deterministically (fast cadence with running-level impact is
        // running, otherwise walking — F03 D3). A failed enter check leaves
        // the exit result standing, which is where the exit hysteresis does
        // its work.
        if (dominant_freq_hz >= c.running_cadence_min_hz &&
            dominant_freq_hz <= c.running_cadence_max_hz && enmo_g >= c.running_enmo_min_g) {
            next = CadenceState::Running;
        } else if (dominant_freq_hz >= c.walking_cadence_min_hz &&
                   dominant_freq_hz <= c.walking_cadence_max_hz && enmo_g >= c.walking_enmo_min_g) {
            next = CadenceState::Walking;
        }
    }

    state_ = next;
    return state_;
}

}  // namespace bw::activity
