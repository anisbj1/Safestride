#pragma once

// bw/wake/alertness_schema.h — the alertness feature set, position for
// position: array index == FeatureVector position (docs/contracts/
// FEATURE_SCHEMA.md §2.1 "order is normative").
//
// This header is F06's hand-maintained mirror of
// `ml/wakefulness/schemas/feature_schema_v1.json` (F06 spec §3, §6 D6). The
// parity test `ml/wakefulness/test_schema_parity.py` fails the build's pytest
// gate if the two ever disagree, so "mirror" cannot rot into "fork".
//
// Why the mirror exists at all: FEATURE_SCHEMA §2.2 puts the generated header
// in `firmware/lib/bw_ml/`, which F06 may not include (01 §1 rule 1) and which
// does not exist yet. F05 D4 reserves the scalable path — the composition
// root injects the generated header into the assembler at M5 — so everything
// behind `buildVector()` is reached through these constants and swaps out in
// one place.

#include <cstdint>

#include <bw/model.h>
#include <bw/state.h>

namespace bw::wake {

// FEATURE_SCHEMA §2.4 registry: id 1 is reserved for the alertness feature
// set, "confirmed when F06 defines its schema". 0 is never issued.
constexpr uint16_t kAlertnessSchemaId = 1;

// Bumped on any change to order, count, definition, unit, source or
// normalization (FEATURE_SCHEMA §2.3). Never reorder without bumping it.
constexpr uint16_t kAlertnessSchemaVersion = 1;

// 16 of the 32 slots bw/model.h allows (kMaxFeatures); headroom for F13's
// personal-baseline features without a schema bump debate.
constexpr uint8_t kAlertnessFeatureCount = 16;

// --- feature order (normative) ---------------------------------------------
// Index: name | unit | source | normalization
//   0 enmo_mean_g          | g        | imu      | standardize(0.05, 0.10)
//   1 enmo_low_ratio       | fraction | imu      | none
//   2 mag_std_mean_g       | g        | imu      | standardize(0.05, 0.05)
//   3 jerk_rms_mean_g_per_s| g_per_s  | imu      | standardize(0.50, 0.50)
//   4 stillness_s          | s        | imu      | standardize(15, 15)
//   5 hr_bpm               | bpm      | ppg      | standardize(72, 12)
//   6 hr_valid_ratio       | fraction | ppg      | none
//   7 hr_z_baseline        | z        | baseline | none
//   8 baseline_valid       | flag     | baseline | none
//   9 activity_stationary  | flag     | activity | none
//  10 activity_confidence  | confidence | activity | none
//  11 tod_sin              | cycle    | clock    | none
//  12 tod_cos              | cycle    | clock    | none
//  13 tod_valid            | flag     | clock    | none
//  14 low_motion_epochs    | count    | history  | none
//  15 enmo_drop_ratio      | fraction | history  | none
//
// Missingness rule (F06 §6 D9): a numeric feature with no data is written at
// its normalization identity (normalized 0 — i.e. the schema mean for
// "standardize", 0 for "none") and a companion flag/ratio feature says so.
// F06 never invents a value, and the model can learn to ignore one.

constexpr const char* kAlertnessFeatureNames[kAlertnessFeatureCount] = {
    "enmo_mean_g",
    "enmo_low_ratio",
    "mag_std_mean_g",
    "jerk_rms_mean_g_per_s",
    "stillness_s",
    "hr_bpm",
    "hr_valid_ratio",
    "hr_z_baseline",
    "baseline_valid",
    "activity_stationary",
    "activity_confidence",
    "tod_sin",
    "tod_cos",
    "tod_valid",
    "low_motion_epochs",
    "enmo_drop_ratio",
};

// [EXPERIMENTAL] Normalization parameters: starting points chosen from the
// plausible ranges of each signal (resting ENMO < 0.1 g, resting HR 72 bpm
// with 12 bpm of spread, ...). Retuned from recorded data by F29 — the JSON
// is the source of truth, this header mirrors it.
constexpr float kAlertnessFeatureMean[kAlertnessFeatureCount] = {
    0.05f, 0.0f, 0.05f, 0.50f, 15.0f, 72.0f, 0.0f, 0.0f,
    0.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f, 0.0f,
};

// Method "none" is rewritten to the identity (mean 0, std 1) at generation
// (FEATURE_SCHEMA §2.2), so firmware applies one formula with no branch.
constexpr float kAlertnessFeatureStd[kAlertnessFeatureCount] = {
    0.10f, 1.0f, 0.05f, 0.50f, 15.0f, 12.0f, 1.0f, 1.0f,
    1.0f,  1.0f, 1.0f,  1.0f,  1.0f,  1.0f,  1.0f, 1.0f,
};

// --- schema index -> explanation -------------------------------------------
//
// F06 §3: contributions become a ReasonMask for F22. Each feature explains
// the reason code its own name promises; features that carry no claim of
// their own (a validity flag, a confidence) map to kNoFeatureReason and never
// set a bit — explaining "baseline_valid contributed" as a reason would be
// nonsense, and 00 §1 forbids inventing meaning from noise.
//
// [ASSUMED] Contributions are reported for the predicted class in feature
// order (FEATURE_SCHEMA §4 works that way); F05 has not landed yet, so this
// is re-checked when IModel ships.

// Sentinel, deliberately outside ReasonCode's range (the enum is append-only
// and has no "none" value — 02 §6.6).
constexpr uint8_t kNoFeatureReason = 0xFF;

constexpr uint8_t kAlertnessFeatureReason[kAlertnessFeatureCount] = {
    static_cast<uint8_t>(ReasonCode::SustainedLowMotion),      // enmo_mean_g
    static_cast<uint8_t>(ReasonCode::SustainedLowMotion),      // enmo_low_ratio
    static_cast<uint8_t>(ReasonCode::SustainedLowMotion),      // mag_std_mean_g
    static_cast<uint8_t>(ReasonCode::SustainedLowMotion),      // jerk_rms
    static_cast<uint8_t>(ReasonCode::ProlongedInactivity),     // stillness_s
    static_cast<uint8_t>(ReasonCode::HrBelowBaseline),         // hr_bpm
    static_cast<uint8_t>(ReasonCode::LowSignalQuality),        // hr_valid_ratio
    static_cast<uint8_t>(ReasonCode::HrBelowBaseline),         // hr_z_baseline
    kNoFeatureReason,                                          // baseline_valid
    static_cast<uint8_t>(ReasonCode::ProlongedInactivity),     // activity_stationary
    kNoFeatureReason,                                          // activity_confidence
    static_cast<uint8_t>(ReasonCode::TimeOfDayTypicalDrowsy),  // tod_sin
    static_cast<uint8_t>(ReasonCode::TimeOfDayTypicalDrowsy),  // tod_cos
    kNoFeatureReason,                                          // tod_valid
    static_cast<uint8_t>(ReasonCode::SustainedLowMotion),      // low_motion_epochs
    static_cast<uint8_t>(ReasonCode::AbruptMotionDrop),        // enmo_drop_ratio
};

// --- generation-time sanity (FEATURE_SCHEMA §2.1, §3) -----------------------
// A bad schema must never reach firmware: id/version 0, a feature over the
// static bound, a non-positive std or a non-finite mean are hard errors here,
// exactly as they are in F05's export_cpp.py.

constexpr bool alertnessSchemaIsWellFormed() {
    if (kAlertnessSchemaId == 0 || kAlertnessSchemaVersion == 0)
        return false;
    if (kAlertnessFeatureCount == 0 || kAlertnessFeatureCount > kMaxFeatures)
        return false;
    for (uint8_t i = 0; i < kAlertnessFeatureCount; ++i) {
        if (kAlertnessFeatureStd[i] <= 0.0f)
            return false;
        // NaN/Inf both fail this comparison (IEEE-754), which is the point.
        if (!(kAlertnessFeatureMean[i] > -1.0e9f && kAlertnessFeatureMean[i] < 1.0e9f))
            return false;
        if (kAlertnessFeatureNames[i] == nullptr || kAlertnessFeatureNames[i][0] == '\0')
            return false;
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < kAlertnessFeatureCount; ++j) {
            const char* a = kAlertnessFeatureNames[i];
            const char* b = kAlertnessFeatureNames[j];
            bool same = true;
            uint8_t k = 0;
            while (a[k] != '\0' && b[k] != '\0') {
                if (a[k] != b[k]) {
                    same = false;
                    break;
                }
                ++k;
            }
            if (same && a[k] != b[k])
                same = false;
            if (same)
                return false;  // names must be unique (§2.1)
        }
    }
    return true;
}

static_assert(alertnessSchemaIsWellFormed(), "FEATURE_SCHEMA §2.1: malformed alertness schema");

}  // namespace bw::wake
