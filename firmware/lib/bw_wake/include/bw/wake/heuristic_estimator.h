#pragma once

// bw/wake/heuristic_estimator.h — F06 D1's heuristic v0 scorer: a transparent
// sum of hand-weighted evidence terms mapped to probabilities by softmax.
//
// Chosen because no labeled narcolepsy data exists (F06 §1, §13), so a
// trained model from day one would be fiction; every term below carries its
// rationale and its ReasonCode (F06 §14 DoD). The trained path is
// ModelAlertnessEstimator behind the same interface (M5).

#include <bw/wake/alertness_estimator.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

class HeuristicAlertnessEstimator final : public AlertnessEstimator {
public:
    explicit HeuristicAlertnessEstimator(const WakeConfig& cfg) : cfg_(cfg) {}

    WakeEstimate estimate(const EpochSummary& epoch, const EpochHistory& history) const override;

    const WakeConfig& config() const { return cfg_; }

private:
    WakeConfig cfg_;
};

}  // namespace bw::wake
