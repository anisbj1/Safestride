// bw/wake/heuristic_estimator.cpp — the transparent v0 scorer (F06 D1).
//
// Every term is: activation in [0,1] -> weight -> one class logit. The term
// table is the feature's documentation (F06 §14: every heuristic term has a
// documented rationale and reason code):
//
//  term            | activation                | class  | reason code
//  ----------------+---------------------------+--------+--------------------
//  t_low           | share of low-motion       | drowsy | SustainedLowMotion
//                  | windows in the epoch      |        |
//  t_active        | 1 - t_low (exact          | awake  | (no claim: motion
//                  | complement)               |        |  present is not a
//                  |                           |        |  "reason for risk")
//  t_inactive      | longest stillness / epoch | drowsy | ProlongedInactivity
//                  | length                    | +sleep |
//  t_hr_low        | baseline z, 0 at the      | drowsy | HrBelowBaseline
//                  | baseline, 1 one MAD below |        |
//  t_hr_drop       | fall vs previous epoch,   | drowsy | HrTrendChange
//                  | capped at kHrTrendDropBpm |        |
//  t_tod           | inside the fixed dip      | drowsy | TimeOfDayTypicalDrowsy
//                  | window                    |        |
//  t_drop          | ENMO lost vs previous     | drowsy | AbruptMotionDrop
//                  | epoch, 0..1               |        |
//  t_sleep_duration| consecutive low-motion    | sleep  | SustainedLowMotion
//                  | epochs / required streak  |        |  + ProlongedInactivity
//
// Absence of a source removes its term (activation 0) instead of assuming a
// value — no baseline means "no HR evidence", never "HR is normal" (F06 §5).

#include <bw/wake/heuristic_estimator.h>

#include <cmath>

#include <bw/wake/wake_math.h>

namespace bw::wake {
namespace {

// Softmax over the three alertness classes, max-shifted for numerical
// safety. Exactly one term is exp(0) = 1, so the sum is >= 1: no division by
// zero, and p_awake + p_drowsy + p_sleep == 1 to within float epsilon.
void softmax3(const float logits[kAlertnessClassCount], float out_prob[kAlertnessClassCount]) {
    float zmax = logits[0];
    if (logits[1] > zmax)
        zmax = logits[1];
    if (logits[2] > zmax)
        zmax = logits[2];
    float sum = 0.0f;
    for (uint8_t i = 0; i < kAlertnessClassCount; ++i) {
        out_prob[i] = std::exp(logits[i] - zmax);
        sum += out_prob[i];
    }
    for (uint8_t i = 0; i < kAlertnessClassCount; ++i) {
        out_prob[i] /= sum;
    }
}

// A term must carry at least kReasonActivationThreshold of its full evidence
// before it is quoted: half-strength terms would make F22 argue for
// something the numbers do not support.
void addReasonIf(ReasonMask& mask, float activation, ReasonCode code, float threshold) {
    if (activation >= threshold) {
        setReason(mask, code);
    }
}

}  // namespace

WakeEstimate HeuristicAlertnessEstimator::estimate(const EpochSummary& e,
                                                   const EpochHistory& history) const {
    WakeEstimate out{};
    out.probs.t_ms = e.t_ms;
    out.probs.valid = false;
    out.probs.input_quality = e.quality;
    out.reasons = qualityReasons(e);

    // Validity gate. imu_usable already carries the quorum (at least
    // min_samples_per_epoch usable IMU windows, feature_assembler.cpp), so a
    // sparse or IMU-dead epoch is "no estimate" with zeroed probabilities —
    // the honest output of 00 §1, not a number to argue with.
    if (!e.imu_usable) {
        return out;
    }

    // F06 §8 (kMinInputQuality): "Insufficient inputs ⇒ valid=false", and
    // F07 discards Insufficient estimates. The epoch's quality is the worst
    // over ALL its windows while the quorum counts only the usable ones, so a
    // mixed epoch (e.g. 20 Good + 10 Insufficient windows) reaches the quorum
    // and still carries Insufficient evidence: probabilities are not
    // published from it. Probabilities stay zeroed, exactly as for a sparse
    // epoch — no number to argue with (00 §1).
    if (e.quality == DataQuality::Insufficient) {
        return out;
    }

    // --- evidence terms (each in [0,1]; clamp01 also rejects NaN) ----------
    const float t_low = clamp01(e.enmo_low_ratio);
    const float t_active = 1.0f - t_low;  // exact complement: the pair makes
                                          // the §9 monotonicity test hold
                                          // by construction

    const float epoch_seconds =
        static_cast<float>(cfg_.epoch_length_ms) / static_cast<float>(kMsPerSecond);
    const float t_inactive =
        epoch_seconds > 0.0f ? clamp01(e.stillness_max_s / epoch_seconds) : 0.0f;

    // HR evidence needs both a baseline and a quorum of valid beats
    // (hrEvidenceUsable): without either there is no term, not a free pass.
    const bool hr_ok = hrEvidenceUsable(e, cfg_);
    const float t_hr_low = hr_ok ? clamp01(e.hr_z_baseline / cfg_.hr_drowsy_z_full) : 0.0f;

    const EpochSummary* prev = previousEpoch(history);
    float t_hr_drop = 0.0f;
    if (prev != nullptr && hr_ok && hrEvidenceUsable(*prev, cfg_) &&
        cfg_.hr_trend_drop_bpm > 0.0f) {
        t_hr_drop = clamp01((prev->hr_mean_bpm - e.hr_mean_bpm) / cfg_.hr_trend_drop_bpm);
    }

    float t_drop = 0.0f;
    if (prev != nullptr) {
        t_drop = enmoDropRatio(e, history);
    }

    const float t_tod = (e.tod_valid && e.tod_min >= cfg_.tod_drowsy_start_min &&
                         e.tod_min < cfg_.tod_drowsy_end_min)
                            ? 1.0f
                            : 0.0f;

    const uint16_t streak = lowMotionEpochStreak(e, history, cfg_);
    const float t_sleep_duration =
        cfg_.sleep_low_motion_epochs > 0
            ? clamp01(static_cast<float>(streak) / static_cast<float>(cfg_.sleep_low_motion_epochs))
            : 0.0f;

    // --- logits -------------------------------------------------------------
    // Weights are relative (softmax ignores a common offset); they are ordered
    // by how much of the epoch each cue covers — motion is the primary
    // modality of this device, time of day is context, not measurement.
    const float logits[kAlertnessClassCount] = {
        cfg_.wake_bias + cfg_.w_active_motion * t_active,
        cfg_.drowsy_bias + cfg_.w_low_motion * t_low + cfg_.w_inactive * t_inactive +
            cfg_.w_hr_below_baseline * t_hr_low + cfg_.w_hr_trend_drop * t_hr_drop +
            cfg_.w_time_of_day * t_tod + cfg_.w_abrupt_motion_drop * t_drop,
        cfg_.sleep_bias + cfg_.w_sleep_duration * t_sleep_duration +
            cfg_.w_sleep_still * t_inactive,
    };

    // Terms are clamp01'd, so a non-finite logit can only come from a corrupt
    // config (a NaN/Inf weight reaching the scorer). exp() would propagate it
    // into the probabilities, so reject it up front: "no estimate" beats a
    // NaN dressed as one (F06 §10: never crash, never publish bad data).
    for (uint8_t i = 0; i < kAlertnessClassCount; ++i) {
        if (!std::isfinite(logits[i])) {
            return out;
        }
    }

    float p[kAlertnessClassCount];
    softmax3(logits, p);

    out.probs.valid = true;
    out.probs.awake = p[0];
    out.probs.drowsy = p[1];
    out.probs.sleep = p[2];
    out.probs.confidence = confidenceCap(e, cfg_);

    // --- reasons (F06 §3: top factors as a ReasonMask for F07/F22) ----------
    addReasonIf(out.reasons, t_low, ReasonCode::SustainedLowMotion,
                cfg_.reason_activation_threshold);
    addReasonIf(out.reasons, t_inactive, ReasonCode::ProlongedInactivity,
                cfg_.reason_activation_threshold);
    addReasonIf(out.reasons, t_hr_low, ReasonCode::HrBelowBaseline,
                cfg_.reason_activation_threshold);
    addReasonIf(out.reasons, t_hr_drop, ReasonCode::HrTrendChange,
                cfg_.reason_activation_threshold);
    addReasonIf(out.reasons, t_drop, ReasonCode::AbruptMotionDrop,
                cfg_.reason_activation_threshold);
    addReasonIf(out.reasons, t_tod, ReasonCode::TimeOfDayTypicalDrowsy,
                cfg_.reason_activation_threshold);
    // Full duration evidence always names both of the cues that produced it.
    if (t_sleep_duration >= cfg_.reason_activation_threshold) {
        setReason(out.reasons, ReasonCode::SustainedLowMotion);
        setReason(out.reasons, ReasonCode::ProlongedInactivity);
    }

    return out;
}

}  // namespace bw::wake
