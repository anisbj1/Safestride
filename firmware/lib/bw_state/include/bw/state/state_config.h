#pragma once

// bw/state/state_config.h — F07 §8 (named constants, every value tagged and
// mirrored in the spec table). Compile-time configuration: StateEngineConfig
// holds every threshold/dwell as a field defaulting to the constant, so tests
// and later F18 settings override fields, never the constants (F03-style
// config, 00 §7.5).

#include <bw/state.h>
#include <bw/types.h>

namespace bw::state {

// ---- probability bands (hysteresis pairs, spec §8, D8) ---------------------

// [EXPERIMENTAL] Evidence at/above which a Drowsy escalation may start its
// dwell. Why 0.60: the hysteresis gap (0.60 enter vs 0.40 exit) stops noise
// around one threshold from flapping the state.
constexpr float kDrowsyEnterProb = 0.60f;

// [EXPERIMENTAL] Evidence at/below which a Drowsy claim is no longer
// supported; also the "calm" line of recovery rule E3 (D8).
constexpr float kDrowsyExitProb = 0.40f;

// [EXPERIMENTAL] Escalation to HighDrowsinessRisk must sit above Drowsy's
// enter; the 0.20 gap matches the Drowsy gap so every rung costs the same
// extra evidence.
constexpr float kHighRiskEnterProb = 0.75f;

// [EXPERIMENTAL] Lands inside Drowsy's band [0.40, 0.60) so E2 de-escalates
// into Drowsy rather than all the way down; gap 0.20.
constexpr float kHighRiskExitProb = 0.55f;

// [EXPERIMENTAL] The `sleep` class alone must dominate before a sleep-like
// claim; implies drowsy+sleep >= 0.75 so the ladder stays nested (spec §7
// bands: a PossibleSleep claim is always reachable from a state
// HighDrowsinessRisk also supports).
constexpr float kSleepEnterProb = 0.85f;

// [EXPERIMENTAL] Above HighRisk's exit, so E2 de-escalates into
// HighDrowsinessRisk; gap 0.20.
constexpr float kSleepExitProb = 0.65f;

// ---- minimum durations (dwell on escalation only, D8/E1) -------------------

// [EXPERIMENTAL] Ignore brief dips: an unbroken run above the Drowsy enter
// threshold before the claim is committed. 15 s (was 1 min; prototype demo
// value, developer 2026-10-18): the demo scenario is "sudden movement, then
// ~15 s of stillness => wakefulness check". The 1 min field-tuned intent is
// the re-tune target at Validate.
constexpr TimeMs kDrowsyMinDurationMs = 15000;

// [EXPERIMENTAL] Escalation needs sustained evidence: 2 min above the
// HighRisk enter threshold.
constexpr TimeMs kHighRiskMinDurationMs = 120000;

// [EXPERIMENTAL] 3 min of sustained sleep-like evidence before claiming
// PossibleSleep — the strongest alertness claim this prototype makes.
constexpr TimeMs kPossibleSleepMinDurationMs = 180000;

// [EXPERIMENTAL] One 1 Hz state step of continuous calm (evidence below the
// Drowsy exit line) before Awake; this demo value makes recovery visible at
// the next state tick. EMA decay still bounds active-motion response to a few
// seconds; tune with replay data before changing demo behavior.
constexpr TimeMs kAwakeRecoveryMs = 1000;

// ---- smoothing (D2/D7) -----------------------------------------------------

// [EXPERIMENTAL] EMA smoothing factor for both evidence channels: 0.2 at
// 1 Hz is ~5 s time constant — fast enough that a 60 s dwell is not eaten by
// lag, slow enough to reject single-sample spikes.
constexpr float kEmaAlpha = 0.2f;

// ---- staleness (per producer, D5) ------------------------------------------

// [EXPERIMENTAL] Wear/activity are 1 Hz producers (runtime §3.2): 5 missed
// windows with task-lateness slack means the axis describes the past -> the
// gate falls to Unknown (§10).
constexpr TimeMs kInputStaleMs = 5000;

// [DERIVED] 2 x the 30 s alertness epoch (runtime §3.3): one late epoch is
// tolerated, two are not. AlertnessProbabilities::valid is the primary
// signal (D5); F06 holds its last answer between epochs and does NOT flip
// valid, so the t_ms check is what bounds staleness (spec §13, corrected
// 2026-10-11).
constexpr TimeMs kAlertnessStaleMs = 60000;

// [EXPERIMENTAL] The feature-quality channel (MotionFeatures/PpgFeatures ->
// SourceQuality) has no staleness field the engine could judge: those
// contracts carry a window t_ms but no publisher-side "this is stale now"
// judgment, and a STOPPED F02 leaves its last Good quality latched in the
// mailbox forever (auditor A-03). 5 missed 1 s windows with task-lateness
// slack (same rationale and value as kInputStaleMs, 1 Hz producers,
// runtime §3.2) ⇒ the feature stream is gone ⇒ the pump ages the channel
// back to Insufficient — missing beats a guess (00 §1). Receipt-time based:
// the pump stamps `now` when a new window is pulled.
constexpr TimeMs kFeatureQualityStaleMs = 5000;

// ---- confidence composition (D9) -------------------------------------------

// [EXPERIMENTAL] A quarter off per degraded source (imu, ppg): two degraded
// sources -> 0.5625 — lower, never zero, so Degraded never looks like
// Unknown. F06's own confidence already carries its input-quality discount,
// so alertness.input_quality is deliberately NOT counted again here (no
// double counting; D9 clarification, status log 2026-10-11).
constexpr float kDegradedConfidenceFactor = 0.75f;

// [DERIVED] Upper bound of 02 §1's [0,1] probability range; saturates
// drowsy+sleep (D13) and clamps composed confidence (D9).
constexpr float kMaxProbability = 1.0f;

// ---- event-record fields (D12) ---------------------------------------------

// [DERIVED] This feature's registry id, written to Event.detector_id so a
// decoder tells a state record from a detector record (F10's kAlertFeatureId
// convention; integrator registry, development documentation).
constexpr uint8_t kStateFeatureId = 7;

// [DERIVED] Event.payload[0] = previous AlertnessState (D12).
constexpr uint8_t kStatePayloadPrevIndex = 0;

// [DERIVED] Event.payload[1] = entered AlertnessState (D12).
constexpr uint8_t kStatePayloadNewIndex = 1;

// [DERIVED] payload[2..7] = 0: reserved, and a zero tail lets a decoder stop
// at the last field it knows (D12).
constexpr uint8_t kStatePayloadReservedBytes = 6;

// [DERIVED] in_state_ms / duration_ms are uint32 (02 §4.5): saturate instead
// of wrapping past 49 days.
constexpr uint32_t kMaxDurationFieldMs = 0xFFFFFFFFu;

// ---- runtime config (D1: enum state + explicit rules, thresholds injected) --

struct StateEngineConfig {
    float drowsy_enter_prob = kDrowsyEnterProb;
    float drowsy_exit_prob = kDrowsyExitProb;
    float high_risk_enter_prob = kHighRiskEnterProb;
    float high_risk_exit_prob = kHighRiskExitProb;
    float sleep_enter_prob = kSleepEnterProb;
    float sleep_exit_prob = kSleepExitProb;
    TimeMs drowsy_min_duration_ms = kDrowsyMinDurationMs;
    TimeMs high_risk_min_duration_ms = kHighRiskMinDurationMs;
    TimeMs possible_sleep_min_duration_ms = kPossibleSleepMinDurationMs;
    TimeMs awake_recovery_ms = kAwakeRecoveryMs;
    float ema_alpha = kEmaAlpha;
    TimeMs input_stale_ms = kInputStaleMs;
    TimeMs alertness_stale_ms = kAlertnessStaleMs;
    float degraded_confidence_factor = kDegradedConfidenceFactor;
};

}  // namespace bw::state
