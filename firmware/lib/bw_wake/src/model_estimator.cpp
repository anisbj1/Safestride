// bw/wake/model_estimator.cpp — IModel glue for the alertness problem.

#include <bw/wake/model_estimator.h>

#include <cmath>

#include <bw/wake/feature_assembler.h>

namespace bw::wake {

WakeEstimate ModelAlertnessEstimator::estimate(const EpochSummary& e,
                                               const EpochHistory& history) const {
    WakeEstimate out{};
    out.probs.t_ms = e.t_ms;
    out.probs.valid = false;
    out.probs.input_quality = e.quality;
    out.reasons = qualityReasons(e);

    // F06 §8 (kMinInputQuality): Insufficient inputs ⇒ valid=false, even
    // when enough usable windows reached the quorum (the epoch's quality is
    // the worst over ALL windows, the quorum counts only the usable ones).
    // No prediction is queried from such an epoch — same rule as the
    // heuristic, so both paths agree on what "no estimate" means.
    if (model_ == nullptr || !e.imu_usable || e.quality == DataQuality::Insufficient) {
        return out;  // no model, no epoch worth predicting, or Insufficient (F06 §5, §8)
    }

    FeatureVector vector{};
    const Result<void> built = buildFeatureVector(cfg_, e, history, e.t_ms, vector);
    if (!built.ok()) {
        return out;
    }

    ModelOutput output{};
    const Result<void> predicted = model_->predict(vector, output);
    if (!predicted.ok() || output.n_classes != kAlertnessClassCount) {
        // Wrong class count means this model is not the alertness model —
        // an error to fall back on, never a probability to reinterpret.
        return out;
    }

    const float cap = confidenceCap(e, cfg_);
    out.probs.valid = true;
    out.probs.awake = output.p[0];
    out.probs.drowsy = output.p[1];
    out.probs.sleep = output.p[2];
    out.probs.confidence = output.confidence < cap ? output.confidence : cap;

    // Top factors -> reason codes (F06 §3). Err::Unsupported is explicitly
    // legal for contributions (bw/model.h), so quality reasons stand alone
    // in that case; mixing a heuristic's explanation into a model's answer
    // would describe a decision the model never made.
    Contribution contributions[kAlertnessFeatureCount];
    const Result<size_t> found =
        model_->contributions(vector, contributions, kAlertnessFeatureCount);
    if (found.ok()) {
        const size_t count =
            found.value < kAlertnessFeatureCount ? found.value : kAlertnessFeatureCount;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t index = contributions[i].feature_index;
            if (index >= kAlertnessFeatureCount) {
                continue;  // defensive: a model cannot explain a feature we do not have
            }
            const float magnitude = std::fabs(contributions[i].weighted_value);
            if (!(magnitude >= cfg_.min_contribution_magnitude)) {
                continue;  // below the quoting floor, or NaN (never >=)
            }
            const uint8_t code = kAlertnessFeatureReason[index];
            if (code != kNoFeatureReason) {
                setReason(out.reasons, static_cast<ReasonCode>(code));
            }
        }
    }

    return out;
}

}  // namespace bw::wake
