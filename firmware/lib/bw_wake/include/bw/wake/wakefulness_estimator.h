#pragma once

// bw/wake/wakefulness_estimator.h — F06's L2 entry point.
//
// One call per 1 s window in, the freshest committed epoch estimate out
// (F06 D2: compute features per second, decide per epoch with a rolling
// window). The epoch rolling lives here rather than in the composition root
// on purpose: wiring stays construction-only (01 §1 rule 3), and the 30 s
// window, the history ring and the estimator choice are behavior, not
// wiring (F06 D7).

#include <bw/clock.h>
#include <bw/model.h>
#include <bw/providers.h>

#include <bw/wake/feature_assembler.h>
#include <bw/wake/heuristic_estimator.h>
#include <bw/wake/model_estimator.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

class WakefulnessEstimator {
public:
    // `clock`, `baseline` and `model` may all be nullptr (AppContext has no
    // device clock before M1, F13 may have no profile, F05 may not have a
    // model): each absence is handled by its own rule, never guessed at.
    // `model` is non-const because bw/model.h's IModel methods are
    // (02 §4.4).
    WakefulnessEstimator(const WakeConfig& cfg, const IClock* clock,
                         const IBaselineProvider* baseline, IModel* model);

    // Folds one window in and returns the freshest committed epoch estimate.
    // Inside an epoch the previous estimate stands (its probs.t_ms names the
    // epoch it belongs to); before the first epoch completes the result is
    // valid == false — the startup transient, not a guess (F06 §5).
    WakeEstimate step(const WakeSample& sample, bool wake_check_has_value = false,
                      TimeMs wake_check_passed_ms = 0, const WearEstimate* wear = nullptr);

    // Epoch/history state and the last estimate (composition root after a
    // pipeline restart, tests).
    void reset();

    const FeatureAssembler& assembler() const { return assembler_; }
    WakeInputRejection lastInputRejection() const { return last_input_rejection_; }

private:
    WakeEstimate demoEstimate(TimeMs t_ms, bool awake, DataQuality quality,
                              bool short_motion_burst) const;
    WakeConfig cfg_;
    IModel* model_;
    FeatureAssembler assembler_;
    HeuristicAlertnessEstimator heuristic_;
    ModelAlertnessEstimator model_estimator_;
    WakeEstimate last_;
    WakeEstimate fast_lane_;
    TimeMs last_sample_ms_;
    TimeMs last_wake_check_ms_;
    TimeMs awake_evidence_ms_ = 0;
    TimeMs initial_contact_ms_ = 0;
    bool have_contact_ = false;
    bool trigger_armed_;
    bool hold_awake_;
    bool first_still_pending_;
    WakeInputRejection last_input_rejection_ = WakeInputRejection::None;
};

}  // namespace bw::wake
