#pragma once

// test/bw_wake/test_bw_wake/wake_test_support.h — private helpers for the
// test/ folder; shared fakes belong to bw_testkit).
//
// Builders are value-initializing (`{}` first) so an unset field is 0/false
// rather than stack garbage, which keeps every test's expectations honest.

#include <bw/clock.h>
#include <bw/model.h>
#include <bw/providers.h>

#include <bw/testkit/fake_clock.h>

#include <bw/wake/feature_assembler.h>
#include <bw/wake/heuristic_estimator.h>
#include <bw/wake/model_estimator.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>
#include <bw/wake/wakefulness_estimator.h>

namespace bw::wake::test {

// Tolerance for probability/normalization comparisons — FEATURE_SCHEMA §4
// uses the same order of magnitude (kParityTolerance 1e-5).
constexpr float kFloatTolerance = 1e-5f;

// --- sample builders --------------------------------------------------------

// One 1 s window at `t_ms`. Defaults describe a healthy, moving window:
// tests change only what they are about.
inline WakeSample makeSample(TimeMs t_ms, float enmo_g,
                             DataQuality imu_quality = DataQuality::Good) {
    WakeSample s{};
    s.t_ms = t_ms;
    s.motion.t_ms = t_ms;
    s.motion.quality = imu_quality;
    s.motion.enmo_g = enmo_g;
    s.motion.mag_std_g = 0.05f;
    s.motion.jerk_rms_g_per_s = 0.4f;
    s.motion.gyro_rms_dps = 5.0f;
    s.motion.gravity_tilt_deg = 45.0f;
    s.motion.dominant_freq_hz = 0.0f;
    s.motion.stillness_ms = enmo_g < 0.03f ? 1000u : 0u;

    s.ppg.t_ms = t_ms;
    s.ppg.quality = DataQuality::Good;
    s.ppg.hr_valid = true;
    s.ppg.hr_bpm = 70.0f;
    s.ppg.hr_confidence = 0.9f;
    s.ppg.perfusion_index = 1.5f;
    s.ppg.ir_dc_level = 50000.0f;
    s.ppg.sqi = 0.9f;
    s.ppg.motion_corrupted = false;
    s.ppg.hrv_valid = false;
    s.ppg.rmssd_ms = 0.0f;

    s.activity.t_ms = t_ms;
    s.activity.context = ActivityContext::Unknown;
    s.activity.confidence = 0.0f;

    s.quality.imu = DataQuality::Good;
    s.quality.ppg = DataQuality::Good;
    return s;
}

// Same window with the PPG side killed (sensor unplugged / gated).
inline WakeSample makeSampleNoPpg(TimeMs t_ms, float enmo_g) {
    WakeSample s = makeSample(t_ms, enmo_g);
    s.ppg.quality = DataQuality::Insufficient;
    s.ppg.hr_valid = false;
    s.ppg.hr_bpm = 0.0f;
    s.quality.ppg = DataQuality::Insufficient;
    return s;
}

// --- epoch builders ---------------------------------------------------------

// A fully-formed epoch: 30 healthy windows, no baseline, no clock. Tests
// override only the field they assert on.
inline EpochSummary makeEpoch(TimeMs t_ms = 30000) {
    EpochSummary e{};
    e.t_ms = t_ms;
    e.sample_count = 30;
    e.imu_usable = true;
    e.ppg_usable = true;
    e.quality = DataQuality::Good;
    e.enmo_mean_g = 0.02f;
    e.enmo_low_ratio = 0.0f;
    e.mag_std_mean_g = 0.05f;
    e.jerk_rms_mean_g_per_s = 0.4f;
    e.stillness_max_s = 0.0f;
    e.hr_mean_bpm = 70.0f;
    e.hr_valid_ratio = 1.0f;
    e.activity_confidence = 0.0f;
    e.activity_dominant = ActivityContext::Unknown;
    e.baseline_valid = false;
    e.hr_z_baseline = 0.0f;
    e.tod_valid = false;
    e.tod_min = 0;
    return e;
}

// Pushes `e` so the invariant "history's newest slot is the current epoch"
// holds (feature_assembler.h). Every estimator test must build history this
// way — it is the same call the assembler makes on commit.
inline void pushHistory(EpochHistory& history, const EpochSummary& e) {
    static_cast<void>(history.push(e));
}

// --- fakes ------------------------------------------------------------------

// IBaselineProvider with an explicit availability switch: `available == false`
// is "F13 is not wired", `p.valid == false` is "F13 has no usable profile" —
// both must behave identically for F06 (02 §5: accept "not available").
struct FakeBaseline final : IBaselineProvider {
    bool available = false;
    BaselineProfile p{};

    bool profile(BaselineProfile& out) const override {
        out = p;
        return available;
    }
};

// Scriptable IModel: lets a test answer, fail, or report a different class
// count, and records the vector it was handed (schema handshake checks).
struct FakeModel final : IModel {
    uint16_t schema_id = kAlertnessSchemaId;
    Err predict_err = Err::Ok;
    ModelOutput output{};
    Err contributions_err = Err::Unsupported;
    Contribution contribution_list[kMaxFeatures]{};
    size_t contribution_count = 0;
    FeatureVector seen{};
    uint32_t predict_calls = 0;

    uint16_t schemaId() const override { return schema_id; }

    Result<void> predict(const FeatureVector& in, ModelOutput& out) override {
        seen = in;
        ++predict_calls;
        if (predict_err != Err::Ok) {
            return {predict_err};
        }
        out = output;
        return {Err::Ok};
    }

    Result<size_t> contributions(const FeatureVector&, Contribution* out, size_t max) override {
        if (contributions_err != Err::Ok) {
            return {contributions_err, 0};
        }
        const size_t n = contribution_count < max ? contribution_count : max;
        for (size_t i = 0; i < n; ++i) {
            out[i] = contribution_list[i];
        }
        return {Err::Ok, n};
    }
};

}  // namespace bw::wake::test
