#pragma once

// bw/wake/wake_config.h — F06 §8: every threshold, weight and factor the
// wakefulness estimator uses, as named tagged constants plus a copyable
// config struct.
//
// All values are starting points (F06 §8); tune with recorded data (F28/F29)
// and log the tuned values back into the F06 spec constants table. The struct
// exists so tests (and later F18 settings) can override thresholds without
// touching code — no globals, config arrives by value (embedded conventions).
//
// Everything here is a number F06 chose; sensor/contract facts live in
// bw_core and hw_config.h instead (F06 §4: bw_core only).

#include <cstdint>

#include <bw/quality.h>

namespace bw::wake {

// --- windows, epochs and history (F06 D2) ----------------------------------

// [EXPERIMENTAL] F02 emits one MotionFeatures/PpgFeatures pair per second
// (bw/features.h §4.2). Why 1 s: it is the actigraphy convention and keeps a
// 30 s epoch at a countable 30 windows.
constexpr uint32_t kFeatureWindowMs = 1000;

// [DERIVED] Actigraphy convention, pinned by F06 §8: 30 s is long enough that
// a few noisy windows cannot dominate (noise averages out) and short enough
// that F07 sees a new estimate before a drowsy episode would matter.
constexpr uint32_t kEpochLengthMs = 30000;

// [EXPERIMENTAL] 10 epochs = 5 min of context (F06 §8); tune with data.
// Compile-time on purpose: it sizes RingBuffer<EpochSummary, kHistoryEpochs>,
// so changing it changes the type — it is a code change, not a setting.
constexpr uint8_t kHistoryEpochs = 10;

// [EXPERIMENTAL] An epoch holding fewer windows than this is reported as
// "no estimate" instead of guessed at (F06 §5, 00 §1): half of the expected
// windows is the least sparse run that still averages to something.
constexpr uint16_t kMinSamplesPerEpoch = 15;

// --- input gating (F06 §8, §10) --------------------------------------------

// [EXPERIMENTAL] A source may be used down to Degraded quality (F06 §8);
// Insufficient contributes nothing and marks the output invalid rather than
// inventing a number from bad data (00 §1).
constexpr DataQuality kMinInputQuality = DataQuality::Degraded;

// [EXPERIMENTAL] Anchor for "sustained low motion" (F06 §8): a 1 s window
// below this ENMO counts as motionless. This is F06's own anchor (0.03 g),
// deliberately NOT the same number as F03's stillness gate (0.02 g) — F06 may
// only depend on bw_core (01 §1 rule 1), so the value is redefined here; do
// not assume F03's classification and F06's low-motion evidence flip at the
// same motion level (tune together at F29).
constexpr float kLowMotionEnmoG = 0.03f;

// [EXPERIMENTAL] User-requested threshold to ignore observed quiet-motion
// noise; only ENMO strictly above 0.04 g qualifies. Tune from measured
// one-second hand-motion vs. stillness windows; epoch anchor stays at 0.03 g.
constexpr float kDemoAwakeMotionEnmoG = 0.04f;

// [EXPERIMENTAL] Initial demo-only strong-burst threshold; 0.35 g is above
// ordinary walking windows (~0.2 g in the synthetic baseline), so the demo
// arms on a distinct burst. Tune with recorded demo traces (F29).
constexpr float kDemoStrongMotionEnmoG = 0.35f;

// [DERIVED] Window-END to consumption: F02 can publish up to one 1 s
// processing tick late, then F06 can consume up to one 1 s tick later.
// Add half a feature window of scheduling slack: 1000 + 1000 + 500 = 2500 ms.
// Bench WAKEGATE measured regular 1992 ms-old windows, not a sensor outage.
constexpr uint32_t kDemoMotionFreshnessMs = 2 * kFeatureWindowMs + kFeatureWindowMs / 2;

// [DERIVED] Consecutive F06 calls remain one 1 s tick + half-window slack.
// Pipeline age must not allow missing ticks to bridge a stillness/burst run.
constexpr uint32_t kDemoInputGapMs = kFeatureWindowMs + kFeatureWindowMs / 2;
// [DERIVED] F04 stamps at production, so only its 1 s consumer phase + slack
// applies; it has no additional F02 window-publication delay to accommodate.
constexpr uint32_t kDemoWearFreshnessMs = kFeatureWindowMs + kFeatureWindowMs / 2;
// [DERIVED] F10 responses are stamped at acceptance: one F06 tick + slack,
// not the sensor pipeline's end-to-end age budget.
constexpr uint32_t kDemoResponseFreshnessMs = kFeatureWindowMs + kFeatureWindowMs / 2;

// [EXPERIMENTAL] A response/movement is recent evidence for five seconds,
// never an indefinite Awake latch. Periodic checks remain F10's responsibility.
constexpr uint32_t kDemoAwakeEvidenceMs = 5000;

// [EXPERIMENTAL] Explicit demo distributions, deliberately decisive for the
// F07 dwell/reset test; probabilities sum to 1 and are not clinical certainty.
constexpr float kDemoDrowsyAwakeProbability = 0.10f;
constexpr float kDemoDrowsyProbability = 0.80f;
constexpr float kDemoDrowsySleepProbability = 0.10f;
constexpr float kDemoAwakeProbability = 0.80f;
constexpr float kDemoAwakeDrowsyProbability = 0.10f;
constexpr float kDemoAwakeSleepProbability = 0.10f;
// [EXPERIMENTAL] IMU-only demo confidence ceiling; reduced because the PPG
// modality is absent, without substituting an HR value. Tune with demo traces.
constexpr float kDemoImuOnlyConfidence = 0.60f;

// [EXPERIMENTAL] Activity evidence is only trusted at or above F03's own
// reporting floor (F03 keeps it for itself, so F06 redefines it — 01 §1).
constexpr float kMinActivityConfidence = 0.5f;

// --- heart-rate evidence ----------------------------------------------------

// [EXPERIMENTAL] The HR term reaches full strength one resting-HR MAD below
// the personal baseline; at/above the baseline (z >= 0) it contributes
// nothing. Why a z-score: 65 bpm is low for one person and normal for
// another, only the baseline-relative value is evidence.
constexpr float kHrDrowsyZFull = -1.0f;

// [EXPERIMENTAL] Floor for resting_hr_mad when F13 reports 0/unknown, so
// z = (hr - resting)/mad cannot divide by zero or explode. 3 bpm is below
// day-to-day resting variation, i.e. the noise floor of the measurement.
constexpr float kMinHrMadFloorBpm = 3.0f;

// [EXPERIMENTAL] Fall in the 30 s mean HR against the previous epoch that
// counts as a downward trend: 5 bpm is ~7 % of a 70 bpm resting rate —
// larger than averaged beat noise, smaller than a posture change.
constexpr float kHrTrendDropBpm = 5.0f;

// --- stillness / duration evidence -----------------------------------------

// [EXPERIMENTAL] Consecutive low-motion epochs needed for full sleep
// evidence: 4 x 30 s = 2 min — long enough to outlast a pause in
// conversation; F07 owns everything about holding a state (F06 §12).
constexpr uint16_t kSleepLowMotionEpochs = 4;

// [EXPERIMENTAL] An epoch is "low motion" once this share of its windows is
// below kLowMotionEnmoG: a majority of the epoch, so one restless window
// does not restart the duration count.
constexpr float kLowMotionEpochRatio = 0.5f;

// --- time of day ------------------------------------------------------------

// [EXPERIMENTAL] Fixed post-lunch dip window, minutes since local midnight.
// 14:00-16:00 is the classic post-prandial dip; F13's personal schedule is
// the scalable replacement, personalisation is out of scope here (F06 §13).
constexpr uint16_t kTodDrowsyStartMin = 840;  // 14:00
constexpr uint16_t kTodDrowsyEndMin = 960;    // 16:00

// [DERIVED] 24 h x 60 min x 60 s — calendar arithmetic used to fold a wall
// clock reading into "minutes since local midnight" (02 §2 has no timezone,
// so the device's unix seconds are assumed to be local — F06 §13 [ASSUMED]).
constexpr uint32_t kSecondsPerDay = 86400;

// [DERIVED] 60 s — unit conversion for the same fold.
constexpr uint32_t kSecondsPerMinute = 60;

// --- evidence quorums -------------------------------------------------------

// [EXPERIMENTAL] HR evidence needs at least this share of the epoch's PPG
// windows to carry a valid heart rate: a mean over two beats out of thirty
// is a guess, not a measurement (F06 §5).
constexpr float kMinHrValidRatio = 0.5f;

// --- clock folding ----------------------------------------------------------

// [DERIVED] 1000 ms — one feature window (unit conversion, F06 D2).
constexpr uint32_t kMsPerSecond = 1000;

// [DERIVED] 60 min x 24 h — the day ring the time-of-day features are laid
// out on (FEATURE_SCHEMA §2.1 units are in the name).
constexpr uint16_t kMinutesPerDay = 1440;

// [DERIVED] 2 x pi, the period of the circular time-of-day encoding. Sin/cos
// of the minute-of-day keeps 23:59 and 00:01 adjacent instead of 1439 and 0
// apart, which a plain "hour / 24" feature would get wrong.
constexpr float kTwoPi = 6.2831853f;

// --- logits: hand weights for the v0 heuristic (F06 D1) ---------------------
//
// Softmax is invariant to a constant added to every logit, so only relative
// sizes matter; the biases fix the "no evidence at all" corner (uniform is
// an honest answer when nothing was observed).

// [EXPERIMENTAL] Awake starts ahead: the device spends most waking hours
// active, and every awake term has to be argued for with evidence.
constexpr float kWakeBias = 1.0f;

// [EXPERIMENTAL] Drowsiness claims require evidence — no free prior.
constexpr float kDrowsyBias = 0.0f;

// [EXPERIMENTAL] Sleep needs duration evidence (kSleepLowMotionEpochs), so it
// starts behind and can only win once the duration term fires.
constexpr float kSleepBias = -1.0f;

// [EXPERIMENTAL] Motion present (1 - low-motion share) is the strongest
// awake cue and is the exact complement of the drowsy motion term, which is
// what makes the monotonicity test in F06 §9 hold by construction.
constexpr float kWActiveMotion = 1.2f;

// [EXPERIMENTAL] Sustained low motion: the primary actigraphy cue for
// drowsiness, weighted equal to its awake complement.
constexpr float kWLowMotion = 1.2f;

// [EXPERIMENTAL] Stillness without duration is weaker than sustained low
// motion (a stationary meeting is not drowsiness).
constexpr float kWInactive = 0.6f;

// [EXPERIMENTAL] HR below the personal baseline: a real autonomic cue, but
// single-modality, so it stays below the motion weights.
constexpr float kWHrBelowBaseline = 0.8f;

// [EXPERIMENTAL] A downward HR trend is a weaker, noisier version of the
// level term (it is a difference of two 30 s means).
constexpr float kWHrTrendDrop = 0.4f;

// [EXPERIMENTAL] Time of day is context, not measurement: it nudges, it
// never decides.
constexpr float kWTimeOfDay = 0.5f;

// [EXPERIMENTAL] An abrupt motion drop is a classic microsleep marker but
// also fires when someone simply stops walking, hence mid-weight.
constexpr float kWAbruptMotionDrop = 0.6f;

// [EXPERIMENTAL] Duration of continuous low motion is what separates sleep
// from "sitting still for 30 s".
constexpr float kWSleepDuration = 1.0f;

// [EXPERIMENTAL] Full stillness inside the epoch supports sleep but is not
// sufficient on its own (F03: never equate low movement with sleep).
constexpr float kWSleepStill = 0.8f;

// --- confidence (F06 §10: degraded input lowers confidence, not honesty) ----

// [EXPERIMENTAL] Ceiling of the heuristic scorer: a hand-weighted rule set
// must never claim certainty; the trained model may report its own value.
constexpr float kBaseConfidence = 0.8f;

// [DERIVED] Good data earns the full ceiling — no penalty for a healthy
// pipeline.
constexpr float kConfidenceGoodFactor = 1.0f;

// [EXPERIMENTAL] Degraded data costs a quarter of the confidence, mirroring
// F03's degraded factor so the pipeline's confidence shrinks consistently.
constexpr float kConfidenceDegradedFactor = 0.75f;

// [EXPERIMENTAL] IMU-only epoch (no PPG): still an estimate, but the
// cardiovascular cue is missing (F06 §10), so it costs a fifth.
constexpr float kConfidencePpgMissingFactor = 0.8f;

// [EXPERIMENTAL] Each optional evidence source that is unavailable (baseline,
// wall clock) costs a tenth: missing evidence must be visible in the
// confidence rather than silently ignored. An absent epoch history is NOT one
// of these factors — it shows up as the history-derived features (low-motion
// streak, ENMO drop ratio) reading 0, which is the honest "no history" value
// (F06 D9: absence lands at the identity, never a penalty on top of it).
constexpr float kConfidenceMissingOptionalFactor = 0.9f;

// --- reporting --------------------------------------------------------------

// [EXPERIMENTAL] A term must carry at least this much of its full evidence
// before it is quoted as a reason: half-strength terms would make F22's
// explanation argue for something the numbers do not support.
constexpr float kReasonActivationThreshold = 0.5f;

// [EXPERIMENTAL] Smallest model contribution worth quoting as a reason;
// below it the "top factor" is noise in a 16-feature vector.
constexpr float kMinContributionMagnitude = 0.1f;

// --- runtime config ---------------------------------------------------------

// Runtime copy of every threshold above (defaults = the named constants).
// Trivially copyable, so it is safe to hold by value and to pass around
// (02 §1). Not every constant is here: the ones that size a type (history
// ring, feature count) are compile-time by construction.
struct WakeConfig {
    // epoching
    uint32_t epoch_length_ms = kEpochLengthMs;
    uint16_t min_samples_per_epoch = kMinSamplesPerEpoch;
    uint32_t feature_window_ms = kFeatureWindowMs;

    // gating
    DataQuality min_input_quality = kMinInputQuality;
    float low_motion_enmo_g = kLowMotionEnmoG;
    float demo_awake_motion_enmo_g = kDemoAwakeMotionEnmoG;
    float min_activity_confidence = kMinActivityConfidence;
    float min_hr_valid_ratio = kMinHrValidRatio;

    // evidence
    float hr_drowsy_z_full = kHrDrowsyZFull;
    float min_hr_mad_floor_bpm = kMinHrMadFloorBpm;
    float hr_trend_drop_bpm = kHrTrendDropBpm;
    uint16_t sleep_low_motion_epochs = kSleepLowMotionEpochs;
    float low_motion_epoch_ratio = kLowMotionEpochRatio;
    uint16_t tod_drowsy_start_min = kTodDrowsyStartMin;
    uint16_t tod_drowsy_end_min = kTodDrowsyEndMin;

    // logits
    float wake_bias = kWakeBias;
    float drowsy_bias = kDrowsyBias;
    float sleep_bias = kSleepBias;
    float w_active_motion = kWActiveMotion;
    float w_low_motion = kWLowMotion;
    float w_inactive = kWInactive;
    float w_hr_below_baseline = kWHrBelowBaseline;
    float w_hr_trend_drop = kWHrTrendDrop;
    float w_time_of_day = kWTimeOfDay;
    float w_abrupt_motion_drop = kWAbruptMotionDrop;
    float w_sleep_duration = kWSleepDuration;
    float w_sleep_still = kWSleepStill;

    // confidence
    float base_confidence = kBaseConfidence;
    float confidence_good_factor = kConfidenceGoodFactor;
    float confidence_degraded_factor = kConfidenceDegradedFactor;
    float confidence_ppg_missing_factor = kConfidencePpgMissingFactor;
    float confidence_missing_optional_factor = kConfidenceMissingOptionalFactor;

    // reporting
    float reason_activation_threshold = kReasonActivationThreshold;
    float min_contribution_magnitude = kMinContributionMagnitude;
};

// [DERIVED] Windows expected in one epoch: epoch length / window length
// (30000/1000 = 30). Used as the completeness denominator, so it follows the
// configured epoch length rather than being a second, drifting constant.
constexpr uint16_t expectedSamplesPerEpoch(const WakeConfig& cfg) {
    // feature_window_ms == 0 is a nonsense config (F06 §10: never crash on
    // invalid input): report "no expectation" instead of dividing by zero.
    // Callers already treat 0 as unknown — confidenceCap reads fill as 0.
    if (cfg.feature_window_ms == 0) {
        return 0;
    }
    return static_cast<uint16_t>(cfg.epoch_length_ms / cfg.feature_window_ms);
}

}  // namespace bw::wake
