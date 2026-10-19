#pragma once

// bw/wear/wear_config.h — F04 §8 (named constants, every value tagged and
// mirrored in the spec table). Compile-time configuration: WearConfig holds
// the four knobs most likely to be tuned first (hysteresis band + dwell);
// the evidence thresholds stay plain constants until F18 settings exist,
// then move behind [USER] keys the same way F02 D9 planned.

#include <bw/types.h>

namespace bw::wear {

// ---- state machine (spec §8) ---------------------------------------------

// [EXPERIMENTAL] Score at/above which a transition to Worn may start its
// dwell. Why: the hysteresis gap (0.7 enter vs 0.4 exit) stops evidence
// noise around one threshold from flapping the state.
constexpr float kWornScoreEnter = 0.7f;

// [EXPERIMENTAL] Score at/below which a transition to NotWorn may start its
// dwell; see kWornScoreEnter for the gap rationale.
constexpr float kWornScoreExit = 0.4f;

// [EXPERIMENTAL] Requires a few seconds of consistent evidence before the
// device claims Worn; a single good-contact window is not enough.
constexpr TimeMs kWornConfirmMs = 5000;

// [EXPERIMENTAL] Brief PPG dropouts during motion must not read as removal,
// so NotWorn needs longer proof than Worn.
constexpr TimeMs kNotWornConfirmMs = 10000;

// Tuning knobs (spec §6 D1): the values a recorded fixture (F28/F29) is most
// likely to move, injectable so tests and later F18 settings share one path.
struct WearConfig {
    float worn_score_enter = kWornScoreEnter;
    float worn_score_exit = kWornScoreExit;
    TimeMs worn_confirm_ms = kWornConfirmMs;
    TimeMs not_worn_confirm_ms = kNotWornConfirmMs;
};

// ---- IMU micro-motion (spec §8) ------------------------------------------

// [EXPERIMENTAL] A device lying on a table shows near-zero variance; below
// this ENMO the motion sub-score is 0. Tune with F28 captures.
constexpr float kDeskMotionEnmoMaxG = 0.005f;

// [EXPERIMENTAL] ENMO of a resting-but-worn limb (postural sway, tremor,
// breathing) clears the desk floor by this much; at/above it a human is
// carrying the device. Why slightly above F02's 0.02 g stillness gate: a
// worn resting user sits between the two, and the score must ramp there
// instead of pinning to 0.
constexpr float kMicroMotionEnmoFullG = 0.03f;

// [EXPERIMENTAL] 5 min of continuous F02 stillness (ENMO < 0.02 g): only a
// device left untouched accumulates minutes of it — a user asleep or awake
// shifts more often than that [ASSUMED] (tune with F28 recordings).
constexpr TimeMs kDeskStillnessMs = 300000;

// ---- PPG contact (spec §8 + D3) ------------------------------------------

// [EXPERIMENTAL] 10 % of the MAX30102 18-bit IR full scale (262143 counts,
// F01's kPpgAdcFullScaleCounts): below it the light reaching the photodiode
// is ambient leakage, not tissue-reflected IR. LED current is set by F01,
// so the absolute counts need a tune with F29 fixtures.
constexpr float kPpgContactIrDcMin = 26214.0f;

// [EXPERIMENTAL] ~25 % of 18-bit full scale: solid skin contact at typical
// LED current; above it more DC adds no further wear evidence.
constexpr float kPpgContactIrDcFull = 65536.0f;

// [EXPERIMENTAL] 0.1 % AC/DC: below it the pulsatile component of this
// sensor is indistinguishable from noise (same floor F02 uses before it
// trusts a beat).
constexpr float kPpgPerfusionMin = 0.001f;

// [EXPERIMENTAL] 0.5 % AC/DC: a firm wrist contact with a clear pulse;
// beyond it perfusion adds little extra wear evidence.
constexpr float kPpgPerfusionFull = 0.005f;

// ---- freshness -----------------------------------------------------------

// [DERIVED] Features are emitted per 1 Hz window (02 §4.2); three missed
// windows means the source has stalled and its values describe the past —
// treat them as absent rather than as evidence.
constexpr TimeMs kFeatureStaleMs = 3000;

// ---- confidence (spec §9, D5) --------------------------------------------

// [EXPERIMENTAL] Ceiling on confidence while only the IMU is available:
// motion alone cannot separate "worn" from "held/carried", so an IMU-only
// run never reports high confidence (spec §9), whatever the score says.
constexpr float kImuOnlyMaxConfidence = 0.6f;

// ---- evidence weights (D4; each group sums to 1) -------------------------

// [EXPERIMENTAL] Source blend: skin contact is direct evidence of wearing,
// motion is circumstantial. Summing to 1 keeps the blend a plain weighted
// mean (no renormalization when both sources are up).
constexpr float kWearPpgWeight = 0.6f;
constexpr float kWearImuWeight = 0.4f;

// [EXPERIMENTAL] PPG sub-weights — contact is the only direct non-wear
// signal, so it carries the largest share; perfusion corroborates blood
// flow under the sensor; a valid HR is derivative of perfusion and fails
// under motion while still worn; SQI is a window-trust summary that partly
// double-counts perfusion, hence the smallest share.
constexpr float kPpgContactWeight = 0.40f;
constexpr float kPpgPerfusionWeight = 0.25f;
constexpr float kPpgHrWeight = 0.20f;
constexpr float kPpgSqiWeight = 0.15f;

// [EXPERIMENTAL] IMU sub-weights — micro-motion is the primary cue (a desk
// device shows near-zero variance); long stillness is secondary
// anti-evidence only, because a resting or sleeping user can be genuinely
// still for minutes.
constexpr float kImuMicroMotionWeight = 0.70f;
constexpr float kImuStillnessWeight = 0.30f;

}  // namespace bw::wear
