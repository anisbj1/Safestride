// bw/cataplexy/candidate_detector.cpp — F09 §7 EventCandidateDetector, spec §6 D1/D2.

#include <bw/cataplexy/candidate_detector.h>

namespace bw::cataplexy {

namespace {

float clampConfidence(float value) {
    if (value < kMinConfidence) {
        return kMinConfidence;
    }
    if (value > kMaxConfidence) {
        return kMaxConfidence;
    }
    return value;
}

}  // namespace

MotionSample EventCandidateDetector::makeSample(const DetectorInput& input) const {
    MotionSample sample;
    sample.t_ms = input.t_ms;

    const MotionFeatures* motion = input.motion;
    sample.valid = motion != nullptr && motion->quality != DataQuality::Insufficient;
    if (motion != nullptr) {
        sample.enmo_g = motion->enmo_g;
        sample.tilt_deg = motion->gravity_tilt_deg;
        sample.jerk_rms_g_per_s = motion->jerk_rms_g_per_s;
    }

    // Usable beat only: an absent or low-confidence estimate is stored as
    // "no HR", so a motion-corrupted window can never corroborate a motion
    // event (00 §1). NaN-free by construction — hr_bpm is only copied when the
    // flags say it is real.
    const PpgFeatures* ppg = input.ppg;
    sample.hr_valid = ppg != nullptr && ppg->quality != DataQuality::Insufficient &&
                      ppg->hr_valid && ppg->hr_confidence >= kMinHrConfidence;
    if (sample.hr_valid) {
        sample.hr_bpm = ppg->hr_bpm;
    }
    return sample;
}

void EventCandidateDetector::reset() {
    for (size_t i = 0; i < kDropHistoryWindows; ++i) {
        history_[i] = MotionSample{};
    }
    count_ = 0;
    last_t_ms_ = 0;
}

void EventCandidateDetector::push(const DetectorInput& input) {
    if (count_ > 0 && (input.t_ms < last_t_ms_ || input.t_ms - last_t_ms_ > kHistoryMaxGapMs)) {
        reset();  // gap or backwards clock: nothing retained describes the present
    }
    for (size_t i = kDropHistoryWindows - 1; i > 0; --i) {
        history_[i] = history_[i - 1];
    }
    history_[0] = makeSample(input);
    if (count_ < kDropHistoryWindows) {
        ++count_;
    }
    last_t_ms_ = input.t_ms;
}

CandidateOutput EventCandidateDetector::step(const DetectorInput& input) {
    CandidateOutput out;
    out.t_ms = input.t_ms;

    // Quality comes from the sources themselves (02 §4.1): an absent source is
    // Insufficient, never a guess.
    out.quality.imu = input.motion != nullptr ? input.motion->quality : DataQuality::Insufficient;
    out.quality.ppg = input.ppg != nullptr ? input.ppg->quality : DataQuality::Insufficient;

    // spec §10: IMU failed => detector disabled, SensorUnavailable. The caller
    // reports a dead IMU by withholding MotionFeatures entirely; a window
    // merely flagged Insufficient is a dropout, handled below as a gate.
    if (input.motion == nullptr) {
        out.disabled = true;
        out.reasons = reasonBit(ReasonCode::SensorUnavailable);
        return out;
    }

    // No snapshot => wear and quality cannot be verified, so nothing is
    // evaluated: confidence stays 0 (00 §1: "insufficient data" is a valid
    // output) and no window is retained, because unworn data must not seed a
    // later candidate.
    if (input.state == nullptr) {
        return out;
    }

    if (input.state->wear != WearState::Worn) {
        reset();  // evidence from an unworn device is meaningless (spec §10/F04)
        out.reasons = reasonBit(ReasonCode::DeviceNotWorn);
        return out;
    }

    out.context = input.state->activity;
    out.alertness_at_start = input.state->alertness;
    push(input);

    if (input.motion->quality == DataQuality::Insufficient) {
        out.reasons = reasonBit(ReasonCode::LowSignalQuality);
        return out;
    }
    const bool degraded = input.motion->quality == DataQuality::Degraded;
    if (degraded) {
        out.reasons = reasonBit(ReasonCode::LowSignalQuality);
    }

    if (count_ < kDropHistoryWindows) {
        return out;  // startup transient: the rule needs the full look-back
    }
    const MotionSample& current = history_[0];
    const MotionSample& previous = history_[1];
    const MotionSample& earlier = history_[2];
    if (!current.valid || !previous.valid) {
        if (!previous.valid) {
            out.reasons = reasonBit(ReasonCode::LowSignalQuality);
        }
        return out;  // no usable window to compare against
    }

    // The rule (spec §8): the arm went from clearly moving to clearly still
    // within kAbruptDropWindowMs. Non-numeric input (NaN) fails both
    // comparisons, so garbage can never trigger.
    const bool abrupt_drop = previous.enmo_g >= config_.active_motion_enmo_g &&
                             current.enmo_g <= config_.post_drop_enmo_g;
    if (!abrupt_drop) {
        return out;
    }

    ConfounderInput confounder_input;
    confounder_input.current = current;
    confounder_input.previous = previous;
    confounder_input.earlier = earlier;
    confounder_input.has_previous = true;
    confounder_input.has_earlier = earlier.valid;
    out.confounders = checkConfounders(confounder_input, config_);
    out.reasons = static_cast<ReasonMask>(out.reasons | reasonBit(ReasonCode::AbruptMotionDrop));

    float confidence = config_.base_confidence;
    if (degraded) {
        confidence *= config_.degraded_penalty;
    }

    // PPG corroboration: the last usable beat before the drop vs the beat in
    // the drop window. Absent on either side simply means no corroboration.
    // Skipped entirely while any confounder flag is set (spec §8): the ×0.4/×0.5
    // penalties must clear kMinCandidateConfidence on their own, so a confounded
    // candidate can never be raised back up by PPG agreeing with it (D2).
    const bool earlier_hr = earlier.hr_valid;
    const bool previous_hr = previous.hr_valid;
    const float pre_drop_bpm = earlier_hr ? earlier.hr_bpm : previous.hr_bpm;
    if (out.confounders == 0 && (earlier_hr || previous_hr) && current.hr_valid &&
        pre_drop_bpm - current.hr_bpm >= config_.hr_drop_bpm) {
        confidence *= config_.hr_corroboration;
        out.reasons = static_cast<ReasonMask>(out.reasons | reasonBit(ReasonCode::HrTrendChange));
    }

    // Confounder penalties: a confounded candidate stays under the raise
    // threshold, and since the boost above is skipped while a confounder is
    // set, no combination of factors can push it back over (spec §8).
    if ((out.confounders & confounderBit(Confounder::FallLike)) != 0) {
        confidence *= config_.fall_penalty;
    }
    if ((out.confounders & confounderBit(Confounder::PostureChange)) != 0) {
        confidence *= config_.posture_penalty;
    }
    if ((out.confounders & confounderBit(Confounder::BriefBurst)) != 0) {
        confidence *= config_.burst_penalty;
    }

    out.confidence = clampConfidence(confidence);
    out.raised = out.confidence >= config_.min_candidate_confidence;
    // The event describes the span the rule judged: the active window it
    // started from through the still window it was confirmed in.
    out.t_start_ms = previous.t_ms;
    out.duration_ms = static_cast<uint32_t>(kAbruptDropWindowMs);
    return out;
}

}  // namespace bw::cataplexy
