#pragma once

// bw/wake/alertness_estimator.h — F06 §7's internal `AlertnessEstimator`
// interface: one epoch in, probabilities + reasons out.
//
// Internal to bw_wake (not a bw_core contract): it exists so the heuristic
// scorer (M2) and the trained model behind IModel (M5) are interchangeable
// behind one call — F06 D1's "replaceable parts" applied to the estimator,
// and the reason F06's wiring never has to change when the model arrives.

#include <bw/wake/feature_assembler.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

class AlertnessEstimator {
public:
    // Estimates one committed epoch. `history` is the assembler's ring whose
    // newest slot is `epoch` itself (feature_assembler.h invariant).
    // A result with probs.valid == false means "no estimate" — probabilities
    // are zeroed, never guessed (F06 §5).
    virtual WakeEstimate estimate(const EpochSummary& epoch, const EpochHistory& history) const = 0;

    // No virtual destructor on purpose, for the same reason bw/clock.h gives:
    // instances are owned by the composition root and never deleted through
    // this pointer, and RTTI is disabled (DEC-002).
};

// Shared confidence ceiling: how much an epoch's data may be trusted, given
// its quality and completeness. Every estimator reports confidence on this
// scale (F06 §10 — degraded input lowers confidence, it never lowers
// honesty), so the heuristic and the model stay comparable.
float confidenceCap(const EpochSummary& epoch, const WakeConfig& cfg);

// Reason bits that describe *data availability* rather than drowsiness:
// reportable by any estimator, and the floor of every reason mask (F06 §3).
ReasonMask qualityReasons(const EpochSummary& epoch);

}  // namespace bw::wake
