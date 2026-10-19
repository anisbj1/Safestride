#pragma once

// firmware/test/bw_cataplexy/test_bw_cataplexy/fixtures.h — shared builders and
// scripted timelines for the F09 suite (private helpers may live in the
//
// Everything here is deterministic: fixed timestamps, fixed values, no clock.

#include <bw/cataplexy/candidate_detector.h>
#include <bw/cataplexy/confounder_checker.h>
#include <bw/cataplexy/event_window_requester.h>

namespace bw::cataplexy::testing {

inline MotionFeatures makeMotion(TimeMs t_ms, float enmo_g, float tilt_deg, float jerk,
                                 DataQuality quality = DataQuality::Good) {
    MotionFeatures motion{};
    motion.t_ms = t_ms;
    motion.quality = quality;
    motion.enmo_g = enmo_g;
    motion.gravity_tilt_deg = tilt_deg;
    motion.jerk_rms_g_per_s = jerk;
    return motion;
}

inline PpgFeatures makePpg(TimeMs t_ms, float hr_bpm, float hr_confidence, bool hr_valid,
                           DataQuality quality = DataQuality::Good) {
    PpgFeatures ppg{};
    ppg.t_ms = t_ms;
    ppg.quality = quality;
    ppg.hr_valid = hr_valid;
    ppg.hr_bpm = hr_bpm;
    ppg.hr_confidence = hr_confidence;
    ppg.sqi = hr_confidence;
    return ppg;
}

inline StateSnapshot makeState(TimeMs t_ms, WearState wear = WearState::Worn,
                               OperatingMode mode = OperatingMode::Day,
                               ActivityContext activity = ActivityContext::Walking) {
    StateSnapshot state{};
    state.t_ms = t_ms;
    state.alertness = AlertnessState::Awake;
    state.wear = wear;
    state.activity = activity;
    state.quality.imu = DataQuality::Good;
    state.quality.ppg = DataQuality::Good;
    state.mode = mode;
    state.power = PowerProfile::Normal;
    state.confidence = 1.0f;
    return state;
}

// Feeds one window: sources present, snapshot present.
inline CandidateOutput stepWindow(EventCandidateDetector& detector, TimeMs t_ms,
                                  const MotionFeatures& motion, const PpgFeatures& ppg,
                                  const StateSnapshot& state) {
    DetectorInput input;
    input.t_ms = t_ms;
    input.motion = &motion;
    input.ppg = &ppg;
    input.state = &state;
    return detector.step(input);
}

// ---- scripted drop timelines (spec §9: 'abrupt drop' vs 'sitting down' vs 'fall') ----

enum class DropScenario {
    Clean,        // sustained activity, arm goes limp: the cataplexy-like case
    SittingDown,  // same drop but the wrist re-oriented (posture confounder)
    Fall,         // uncontrolled descent with a jerk spike (fall confounder)
    BriefBurst,   // a short gesture that stopped (too little prior activity)
    HrDrop,       // clean drop that PPG independently corroborates
};

struct DropTimeline {
    MotionFeatures earlier;
    MotionFeatures previous;
    MotionFeatures current;
    PpgFeatures ppg_earlier;
    PpgFeatures ppg_previous;
    PpgFeatures ppg_current;
    StateSnapshot state;
};

// Three windows at the 1 Hz feature cadence: two active ones, then the still
// window in which the drop is observed. Every scenario triggers the rule; they
// differ only in the confounders/corroboration attached to it.
inline DropTimeline makeDropTimeline(DropScenario scenario) {
    const TimeMs t_now = 60000;

    DropTimeline timeline;
    timeline.earlier = makeMotion(t_now - 2 * kFeatureWindowMs, 0.26f, 30.0f, 0.5f);
    timeline.previous = makeMotion(t_now - kFeatureWindowMs, 0.24f, 30.0f, 0.6f);
    timeline.current = makeMotion(t_now, 0.01f, 31.0f, 0.2f);

    // PPG off by default: the rule must work without a usable beat (00 §1).
    timeline.ppg_earlier = makePpg(t_now - 2 * kFeatureWindowMs, 0.0f, 0.0f, false);
    timeline.ppg_previous = makePpg(t_now - kFeatureWindowMs, 0.0f, 0.0f, false);
    timeline.ppg_current = makePpg(t_now, 0.0f, 0.0f, false);

    timeline.state = makeState(t_now);

    switch (scenario) {
    case DropScenario::Clean:
        break;
    case DropScenario::SittingDown:
        // The arm did not stay where it was: 45° of wrist rotation.
        timeline.current.gravity_tilt_deg = 75.0f;
        break;
    case DropScenario::Fall:
        // Uncontrolled descent: jerk well above the fall threshold.
        timeline.previous.jerk_rms_g_per_s = 4.0f;
        break;
    case DropScenario::BriefBurst:
        // The window before the active one was quiet: the motion was short.
        timeline.earlier.enmo_g = 0.02f;
        break;
    case DropScenario::HrDrop:
        timeline.ppg_earlier = makePpg(t_now - 2 * kFeatureWindowMs, 72.0f, 0.9f, true);
        timeline.ppg_previous = makePpg(t_now - kFeatureWindowMs, 72.0f, 0.9f, true);
        timeline.ppg_current = makePpg(t_now, 60.0f, 0.9f, true);
        break;
    }
    return timeline;
}

// Runs the whole timeline and returns the conclusion of the drop window.
inline CandidateOutput runDrop(EventCandidateDetector& detector, DropScenario scenario) {
    const DropTimeline timeline = makeDropTimeline(scenario);
    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);
    return stepWindow(detector, timeline.current.t_ms, timeline.current, timeline.ppg_current,
                      timeline.state);
}

inline bool hasConfounder(ConfounderMask mask, Confounder confounder) {
    return (mask & confounderBit(confounder)) != 0;
}

}  // namespace bw::cataplexy::testing
