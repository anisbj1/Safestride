#pragma once

// bw/activity/activity_config.h — F03 §8: every threshold the activity
// classifier uses, as named tagged constants plus a copyable config struct.
//
// All values are starting points (F03 §8); tune with recorded data (F28/F29)
// and log the tuned values back into the F03 spec constants table. The
// struct exists so tests (and later F18 settings) can override thresholds
// without touching code — no globals, config arrives by value (embedded
// conventions).

#include <cstdint>

namespace bw::activity {

// --- cadence bands (D1/D3) --------------------------------------------------

// [DERIVED] Typical human step cadence 72–168 steps/min = 1.2–2.8 Hz.
constexpr float kWalkingCadenceMinHz = 1.2f;

// [DERIVED] Upper end of typical walking cadence (168 steps/min).
constexpr float kWalkingCadenceMaxHz = 2.8f;

// [DERIVED] Running cadence ≳156 steps/min = 2.6 Hz. Overlaps the walking
// band (2.6–2.8 Hz); kRunningEnmoMinG below resolves the overlap (F03 D3).
constexpr float kRunningCadenceMinHz = 2.6f;

// [DERIVED] 240 steps/min — above any sustained human cadence (elite sprint
// tops out ≈4 Hz); higher frequencies are vibration/noise, not running.
constexpr float kRunningCadenceMaxHz = 4.0f;

// [EXPERIMENTAL] Exit-band widening (≈12 steps/min) so a frequency hovering
// at a band edge cannot toggle CadenceState every window (F03 D4).
constexpr float kCadenceHysteresisHz = 0.2f;

// [EXPERIMENTAL] Step cadence without whole-body motion is tapping/fidget,
// not walking (F03 D3).
constexpr float kWalkingEnmoMinG = 0.03f;

// [EXPERIMENTAL] Separates running from fast walking inside the 2.6–2.8 Hz
// overlap: running impact is several × walking (F03 D3).
constexpr float kRunningEnmoMinG = 0.20f;

// --- stillness / posture (D2/D6) -------------------------------------------

// [EXPERIMENTAL] "No meaningful motion" for the still gate; deliberately at
// F02's stillness enter level so both stages flip at the same motion level.
// Redefined here because F03 may only depend on bw_core (01 §1 rule 1).
constexpr float kStationaryEnmoMaxG = 0.02f;

// [EXPERIMENTAL] 10 min of continuous stillness: long enough to exclude
// pauses in conversation; tune with data. Never implies sleep (F03 §5).
constexpr uint32_t kProlongedInactivityMs = 600000;

// [EXPERIMENTAL] ±20° around horizontal for the LyingResting row: wrist
// orientation noise while the arm is still.
constexpr float kLyingTiltMinDeg = 70.0f;
constexpr float kLyingTiltMaxDeg = 110.0f;

// --- confidence (D5) ---------------------------------------------------------

// [EXPERIMENTAL] Reporting floor: below this a classification is reported
// as Unknown with confidence 0 (F03 §8).
constexpr float kMinConfidenceToReport = 0.5f;

// [EXPERIMENTAL] Posture cap = floor: LyingResting is the weakest
// reportable claim, so one Degraded factor drops it below the floor (D2/D5).
constexpr float kPostureConfidenceCap = 0.5f;

// [EXPERIMENTAL] A quarter off every confidence under Degraded IMU data:
// robust rows stay ≥ floor (0.7 → 0.525), posture falls to Unknown (D5).
constexpr float kDegradedConfidenceFactor = 0.75f;

// [EXPERIMENTAL] Base confidence per class, ordered by cue strength
// (two agreeing cues > one cue). Placeholders for an F05 posterior later.
constexpr float kConfStationary = 0.7f;         // single cue: stillness
constexpr float kConfWalking = 0.75f;           // cadence band + energy
constexpr float kConfRunning = 0.8f;            // narrow band + impact
constexpr float kConfInactiveProlonged = 0.7f;  // duration evidence

// --- temporal smoothing (D4) -------------------------------------------------

// [EXPERIMENTAL] Context changes must hold this many consecutive 1 Hz
// windows: rejects 1–2 window glitches, a real transition still appears
// within 3 s. Insufficient quality bypasses it (spec §10).
constexpr uint8_t kMinDwellWindows = 3;

// [DERIVED] Expected 1 s window + 50 % scheduling slack; a larger jump means
// windows were lost, so dwell evidence is not continuous → reset dwell.
constexpr uint32_t kMaxFeatureStepGapMs = 1500;

// Runtime copy of every threshold. Defaults are the named constants above;
// trivially copyable, so it is safe to hold by value in the classifier and
// to pass through queues/mailboxes (02 §1).
struct ActivityConfig {
    float walking_cadence_min_hz = kWalkingCadenceMinHz;
    float walking_cadence_max_hz = kWalkingCadenceMaxHz;
    float running_cadence_min_hz = kRunningCadenceMinHz;
    float running_cadence_max_hz = kRunningCadenceMaxHz;
    float cadence_hysteresis_hz = kCadenceHysteresisHz;
    float walking_enmo_min_g = kWalkingEnmoMinG;
    float running_enmo_min_g = kRunningEnmoMinG;
    float stationary_enmo_max_g = kStationaryEnmoMaxG;
    uint32_t prolonged_inactivity_ms = kProlongedInactivityMs;
    float lying_tilt_min_deg = kLyingTiltMinDeg;
    float lying_tilt_max_deg = kLyingTiltMaxDeg;
    float min_confidence_to_report = kMinConfidenceToReport;
    float posture_confidence_cap = kPostureConfidenceCap;
    float degraded_confidence_factor = kDegradedConfidenceFactor;
    float conf_stationary = kConfStationary;
    float conf_walking = kConfWalking;
    float conf_running = kConfRunning;
    float conf_inactive_prolonged = kConfInactiveProlonged;
    uint8_t min_dwell_windows = kMinDwellWindows;
    uint32_t max_feature_step_gap_ms = kMaxFeatureStepGapMs;
};

}  // namespace bw::activity
