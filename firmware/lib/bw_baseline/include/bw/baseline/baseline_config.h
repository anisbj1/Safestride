#pragma once

// bw/baseline/baseline_config.h — F13 §8: every named, tagged constant of the
// feature plus BaselineConfig (the injectable subset most likely to be tuned
// later, F18 settings / F29 data). Values mirror the spec §8 table; all are
// starting points unless tagged [HW].

#include <bw/context.h>
#include <bw/types.h>

#include <cstdint>

namespace bw::baseline {

// ---- evidence / validity ----------------------------------------------------

// [EXPERIMENTAL] Minimum evidence before `valid = true` (original
// kMinQualifiedMinutes = 30, expressed in ms as the spec §8 table pins it).
constexpr uint32_t kMinQualifiedMs = 1800000;

// [DERIVED] One accepted sample is one F02 1 Hz window = 1000 ms; converts the
// u32 sample counter into the ms unit kMinQualifiedMs is stated in
// (1'800'000 ms / 1000 ms-per-sample = 1800 samples, §9 test 2).
constexpr uint32_t kMsPerSecond = 1000;

// [EXPERIMENTAL] 2 h of qualified evidence (7200 samples at 1 Hz) raises
// confidence_pct to 100; below it the ramp is linear (§8, §9 test 14).
constexpr uint32_t kFullConfidenceSamples = 7200;

// [DERIVED] Percent full scale — the unit of BaselineProfile::confidence_pct
// (02 §5): used as the ramp numerator and the clamp so the two cannot drift.
constexpr uint32_t kConfidencePctMax = 100;

// ---- EWMA estimator (D1) ----------------------------------------------------

// [EXPERIMENTAL] Slow drift tracking; the original constant. The learner's
// time constant this derives from — see kEwmaSteadyWindowSamples.
constexpr uint32_t kAdaptationTimeConstantH = 48;

// [EXPERIMENTAL] DERIVED from kAdaptationTimeConstantH = 48 h: ~4 days x
// ~7200 qualified s/day at 1 Hz = 28800, rounded to 30000; EWMA alpha is
// 1/this after the ramp (§8).
constexpr uint32_t kEwmaSteadyWindowSamples = 30000;

// [EXPERIMENTAL] 10 min at 1 Hz: incremental-mean seeding (alpha = 1/(n+1))
// converges to the true mean fast; the outlier gate stays off until then (D1).
constexpr uint32_t kEwmaRampSamples = 600;

// [EXPERIMENTAL] Reject samples beyond 4 robust deviations from the current
// estimate — the post-ramp drift/artifact guard (D1, §9 test 4).
constexpr float kOutlierBandSigma = 4.0f;

// [EXPERIMENTAL] Absolute floor of the outlier band (bpm) so a near-zero early
// MAD cannot lock learning out (D1, §9 test 4 boundary rows).
constexpr float kMinOutlierBandBpm = 8.0f;

// [EXPERIMENTAL] Physiological plausibility band for resting HR (bpm);
// outside it the sample is artifact, not rest (§9 test 9). Written as one
// inclusive band: the NaN-hr check is the negation of these two comparisons.
constexpr float kMinRestingHrBpm = 35.0f;
constexpr float kMaxRestingHrBpm = 130.0f;

// ---- activity gates (D2) ----------------------------------------------------

// [EXPERIMENTAL] Resting HR requires rest (original constant): the normal
// learning gate.
constexpr ActivityContext kQualifiedActivity[] = {
    ActivityContext::Stationary,
    ActivityContext::Sitting,
};

// [EXPERIMENTAL] D2 gate widening during a guided Calibration session: a
// misclassified still posture (Standing/LyingResting) still counts. Wear and
// quality gates stay hard in Calibration — only the activity set widens.
constexpr ActivityContext kQualifiedActivityCalibration[] = {
    ActivityContext::Stationary,
    ActivityContext::Sitting,
    ActivityContext::Standing,
    ActivityContext::LyingResting,
};

// ---- poisoning guard (D5) ---------------------------------------------------

// [EXPERIMENTAL] 10 min cooldown at 1 Hz after a suspected event/episode hold
// clears, before learning resumes (§5 rule 2, §9 test 8).
constexpr uint32_t kPostEventHoldSamples = 600;

// ---- persistence (D3) -------------------------------------------------------

// [DERIVED] BaselineProfile::version = meaning-version of the profile; mirrors
// the blob schema v1 (§8).
constexpr uint16_t kProfileVersion = 1;

// [DERIVED] Blob magic 'F13B' — when written little-endian the on-flash bytes
// read F 1 3 B (§8, corruption/format detection).
constexpr uint32_t kBlobMagic = 0x42313346u;

// [DERIVED] Blob layout version; an unknown schema is Err::Unsupported and is
// discarded exactly like a corrupt blob (D3, §10).
constexpr uint16_t kBlobSchemaV1 = 1;

// [DERIVED] Blob size in bytes (§8/§11): 4 magic + 2 schema + 2 flags + 12
// three f32 + 4 samples + 4 reserved + 4 crc32 = 32 B < the 200 B flash budget.
constexpr uint16_t kBlobBytes = 32;

// [DERIVED] Stable IKeyValueStore key; feature-prefixed, never renumbered (§8).
constexpr const char kStoreKey[] = "f13.baseline";

// [EXPERIMENTAL] Flash-write cadence: one KV put per 10 min of learning =
// ~144 writes/day (trivial wear), plus the valid false→true transition (§8,
// D3; the pump applies it — a failed put is retried at the next interval).
constexpr TimeMs kPersistIntervalMs = 600000;

// ---- pump freshness (§7) ----------------------------------------------------

// [EXPERIMENTAL] Pump freshness guard: PPG/motion windows older than 5 s are
// stale, not evidence (§8, freshness is the pump's job — F06 precedent).
constexpr TimeMs kMaxSampleAgeMs = 5000;

// ---- compile-time configuration --------------------------------------------

// The knobs a first tuning round (F28/F29) or F18 settings is most likely to
// move, injectable through BaselineTracker's constructor; defaults are the §8
// constants above, so default-constructed trackers behave per spec.
struct BaselineConfig {
    uint32_t min_qualified_ms = kMinQualifiedMs;
    uint32_t full_confidence_samples = kFullConfidenceSamples;
    uint32_t ewma_steady_window_samples = kEwmaSteadyWindowSamples;
    uint32_t ewma_ramp_samples = kEwmaRampSamples;
    float outlier_band_sigma = kOutlierBandSigma;
    float min_outlier_band_bpm = kMinOutlierBandBpm;
    float min_resting_hr_bpm = kMinRestingHrBpm;
    float max_resting_hr_bpm = kMaxRestingHrBpm;
    uint32_t post_event_hold_samples = kPostEventHoldSamples;
};

}  // namespace bw::baseline
