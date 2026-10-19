#pragma once

// bw/wake/model_estimator.h — the trained path (F06 §7, M5): wraps IModel
// (F05) behind the same internal interface the heuristic implements, so the
// scorer can be swapped without touching the wiring (F06 D1: heuristic v0
// now, trained linear/tree model behind IModel later).
//
// Schema validation is deliberately NOT done here: IModel::predict compares
// schema id/version/count and rejects NaN itself (FEATURE_SCHEMA §3), which
// keeps one implementation of that rule in the whole system. This class maps
// classes, confidence and contributions onto F06's output.

#include <bw/model.h>

#include <bw/wake/alertness_estimator.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

class ModelAlertnessEstimator final : public AlertnessEstimator {
public:
    // `model` may be nullptr (no F05 runtime yet): the estimator then reports
    // "no estimate" and the caller falls back — it never invents a posterior.
    // Non-const on purpose: bw/model.h's IModel::predict/contributions are
    // non-const in the locked contract (02 §4.4), so F06 consumes a plain
    // pointer — raising a CR to const-qualify them is F05's call, not a
    // reason for F06 to edit a contract.
    ModelAlertnessEstimator(IModel* model, const WakeConfig& cfg) : model_(model), cfg_(cfg) {}

    // Returns probs.valid == false when the model is absent, the vector
    // cannot be built, predict() fails, or the model is not a 3-class
    // alertness model. Callers treat that as "use the heuristic".
    WakeEstimate estimate(const EpochSummary& epoch, const EpochHistory& history) const override;

private:
    IModel* model_;
    WakeConfig cfg_;
};

}  // namespace bw::wake
