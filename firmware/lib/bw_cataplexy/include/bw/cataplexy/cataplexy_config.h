#pragma once

// bw/cataplexy/cataplexy_config.h — F09 §8 (named constants, every value tagged
// and mirrored in the spec table) plus the tuning struct the detector, the
// confounder checker and the tests share.
//
// F09 is an experimental candidate detector + data-capture tool (00 §1): every
// threshold below is a starting point until F28/F29 recordings exist. Each
// value carries its tag and the reasoning — never a bare number (00 §7.5).

#include <cstddef>
#include <cstdint>

#include <bw/state.h>
#include <bw/types.h>

namespace bw::cataplexy {

// Bit for a single ReasonCode (02 §4.5). Kept feature-local on purpose: a
// feature may only include bw_core headers (01 §1 rule 1), so the same one-line
// helper exists in each feature that needs it.
constexpr ReasonMask reasonBit(ReasonCode code) {
    return ReasonMask{1} << static_cast<uint8_t>(code);
}

// ---- input cadence ---------------------------------------------------------

// [DERIVED] F02 emits MotionFeatures and PpgFeatures once per 1 s window
// (02 §4.2), so one detector step == one second of evidence and the drop rule's
// times can be expressed in whole windows.
constexpr TimeMs kFeatureWindowMs = 1000;

// ---- abrupt-drop rule (spec §8 + D1) ---------------------------------------

// [EXPERIMENTAL] (spec §8) Window in which a sudden loss of arm motion is
// evaluated. Why 2 s: long enough to contain "moving" and "still" windows at
// the 1 Hz feature cadence, short enough that a deliberate lowering of the arm
// (a second or two of controlled motion) is unlikely to fit inside it.
constexpr TimeMs kAbruptDropWindowMs = 2000;

// [DERIVED] Windows kept by the detector: the current one, the window the drop
// started from, and one more back to tell sustained activity (cataplexy-like)
// from a short burst (gesture). ceil(kAbruptDropWindowMs / kFeatureWindowMs) + 1.
constexpr size_t kDropHistoryWindows = 3;

// [DERIVED] Largest gap the retained windows may have between them. Why
// kAbruptDropWindowMs: the rule claims the motion stopped *within* that window,
// so one missed feature window (a 2 s gap) still supports the claim while two
// missed windows (3 s) do not — the history restarts instead of comparing
// across the gap. A backwards timestamp triggers the same restart (see the
// detector).
constexpr TimeMs kHistoryMaxGapMs = kAbruptDropWindowMs;

// [EXPERIMENTAL] ENMO at/above which the arm was demonstrably moving before
// the drop. Why 0.10 g: F04 places a worn resting limb around 0.03 g of
// micro-motion while ambulatory arm swing is several times that; 0.10 g sits
// between "resting" and "walking", so a user who was already quiet is never
// scored as having lost motion.
constexpr float kActiveMotionEnmoG = 0.10f;

// [EXPERIMENTAL] ENMO at/below which the arm counts as still after the drop —
// the resting-limb band (postural sway). Why not lower: a device lying
// untouched reads below 0.01 g, and requiring that would reject a genuinely
// limp worn arm.
constexpr float kPostDropEnmoG = 0.03f;

// ---- confounders (spec §6 D2) ----------------------------------------------

// [EXPERIMENTAL] Jerk RMS in the active window at/above which the descent
// reads as uncontrolled (fall) rather than a controlled release of the arm.
// Why 2.0 g/s: a 0.5 s arrest of a ~5 m/s fall is ≈ 10 m/s³ ≈ 1 g/s of mean
// jerk, and the RMS over the window peaks well above the mean.
constexpr float kFallJerkRmsGPerS = 2.0f;

// [EXPERIMENTAL] ENMO in the active window at/above which the impact of a fall
// shows as a large excess over 1 g during the window; an arm swinging under
// control stays far below this.
constexpr float kFallPeakEnmoG = 0.8f;

// [EXPERIMENTAL] Change of the gravity-to-z angle across the drop at/above
// which the wrist re-oriented (sitting down, lying the arm down): a posture
// change, not a loss of tone with the arm where it was. Why 30°: a seated
// transition rotates the forearm by roughly 45–90°, 30° leaves margin for
// sensor mounting variation while a limp arm keeps its angle.
constexpr float kPostureTiltDeltaDeg = 30.0f;

// ---- PPG corroboration (spec §3 inputs) ------------------------------------

// [EXPERIMENTAL] Drop in heart rate from the last valid pre-event window to
// the drop window at/above which PPG corroborates the candidate. Why 8 bpm:
// larger than normal beat-to-beat variation between 1 s windows, smaller than
// the swing a motion artefact would produce — and artefacts are filtered out
// by kMinHrConfidence first.
constexpr float kHrDropBpm = 8.0f;

// [EXPERIMENTAL] PPG beat confidence below which hr_bpm is treated as absent:
// a motion-corrupted estimate must never corroborate a motion event (00 §1).
constexpr float kMinHrConfidence = 0.5f;

// ---- confidence (spec §8, D2/D4) -------------------------------------------

// [EXPERIMENTAL] Confidence of a clean abrupt drop with no confounders. Why
// 0.75: it clears kMinCandidateConfidence with room for the degraded-quality
// penalty, while any single motion confounder (×0.4…0.5) lands below the raise
// threshold — so confounders demote the candidate to the research stream
// instead of raising an event (spec §8).
constexpr float kBaseCandidateConfidence = 0.75f;

// [EXPERIMENTAL] (spec §8) Below this, do not raise an event; log to the
// research stream only.
constexpr float kMinCandidateConfidence = 0.4f;

// [EXPERIMENTAL] Confidence penalties per confounder bit; factors multiply when
// several apply (a fall that also changed posture is the least likely of all).
constexpr float kFallPenaltyFactor = 0.4f;
constexpr float kPostureChangePenaltyFactor = 0.5f;
constexpr float kBriefBurstPenaltyFactor = 0.5f;

// [EXPERIMENTAL] Motion window flagged Degraded by F23 still counts, but the
// evidence is weaker, so the score is trimmed (00 §1: quality gating is
// mandatory, not optional).
constexpr float kDegradedQualityPenaltyFactor = 0.8f;

// [EXPERIMENTAL] Boost when PPG independently shows the HR trend that
// accompanies loss of tone. Why 1.15: corroboration should reward a multimodal
// match without letting it single-handedly turn a confounded candidate into an
// event — the factor is applied only to confounder-free candidates and is
// skipped entirely whenever any confounder flag is set (spec §8), so the ×0.4
// and ×0.5 penalties below always clear the raise threshold on their own.
constexpr float kHrCorroborationFactor = 1.15f;

// [DERIVED] Clamp range — 02 §1 fixes probabilities/confidences to [0,1], and
// multiplying factors must not escape it.
constexpr float kMaxConfidence = 1.0f;
constexpr float kMinConfidence = 0.0f;

// ---- event-window capture (spec §8) ----------------------------------------

// [EXPERIMENTAL] (spec §8) Context before the user's marker.
constexpr TimeMs kPreEventCaptureMs = 30000;

// [EXPERIMENTAL] (spec §8) Context after the user's marker; the request is
// emitted only once this has elapsed, so the captured span is complete.
constexpr TimeMs kPostEventCaptureMs = 30000;

// [DERIVED] Captures in flight. Why 1: pre+post span kPreEventCaptureMs +
// kPostEventCaptureMs (60 s) of ring buffer, and a second mark inside that
// span describes the same episode; keeping the first mark is the cheapest way
// to stay inside F18's per-window budget (spec §11).
constexpr size_t kMaxPendingCaptures = 1;

// ---- Event::payload layout for EventType::SuspectedCataplexy ---------------

// F09-local layout of the 8 payload bytes (02 §4.5 defines payload as
// "type-specific", so the owning detector pins it):
//   payload[0] bit0 = raw-window request: F18 dumps its ring buffer around the
//                     event when it stores it (spec §3 "event-window API")
//   payload[1]      = ConfounderMask (local adapter until CR-009 lands the
//                     codes in ReasonCode — see D4 in the spec)
//   payload[2..7]   = reserved, written as 0
// [EXPERIMENTAL] (spec §8 tags the flag this way) The byte layout and the bit
// value are F09's proposal, not a contract: bit0 stays a *request* that F18 may
// gate by OperatingMode (D3) and payload[1] disappears once CR-009 appends the
// reason codes (D4), so nothing here is frozen.
constexpr size_t kPayloadWindowFlagIndex = 0;
constexpr size_t kPayloadConfounderIndex = 1;
constexpr uint8_t kWindowRequestFlag = 0x01;

// [ASSUMED] Event::detector_id namespace: bw_core reserves the field but F00
// defined no registry (core.md §7 leaves it to the owning features). 9 == F09;
// verify with the integrator when F17/F22 consume the field.
constexpr uint8_t kCataplexyDetectorId = 9;

// ---- tuning knobs (spec §6 D4) ---------------------------------------------

// The values F29 is most likely to move, injectable so tests and later [USER]
// settings (F18) share one path instead of editing call sites.
struct CataplexyConfig {
    float active_motion_enmo_g = kActiveMotionEnmoG;
    float post_drop_enmo_g = kPostDropEnmoG;
    float fall_jerk_g_per_s = kFallJerkRmsGPerS;
    float fall_peak_enmo_g = kFallPeakEnmoG;
    float posture_tilt_delta_deg = kPostureTiltDeltaDeg;
    float hr_drop_bpm = kHrDropBpm;
    float base_confidence = kBaseCandidateConfidence;
    float min_candidate_confidence = kMinCandidateConfidence;
    float fall_penalty = kFallPenaltyFactor;
    float posture_penalty = kPostureChangePenaltyFactor;
    float burst_penalty = kBriefBurstPenaltyFactor;
    float degraded_penalty = kDegradedQualityPenaltyFactor;
    float hr_corroboration = kHrCorroborationFactor;
};

}  // namespace bw::cataplexy
