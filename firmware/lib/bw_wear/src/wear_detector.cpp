#include <bw/wear/wear_detector.h>

namespace bw::wear {

WearEstimate WearDetector::step(const WearInput& input) {
    const WearEvidence evidence = evaluateWearEvidence(input.ppg, input.motion, input.t_ms);

    // §10: neither source has usable values (stall, dropout, absent fakes).
    // There is no evidence to accumulate a dwell over, so Uncertain is
    // immediate — and confidence 0, we claim nothing.
    if (!evidence.ppg_available && !evidence.imu_available) {
        state_ = WearState::Uncertain;
        resetCandidate();
        return WearEstimate{input.t_ms, WearState::Uncertain, 0.0f};
    }

    const WearState candidate = candidateState(combinedScore(evidence));
    if (candidate == state_) {
        resetCandidate();
    } else {
        // Monotonic time moving backwards (reboot, wrap): the pending dwell
        // references a different epoch, so restart it instead of letting
        // the unsigned subtraction commit instantly.
        if (candidate_pending_ && input.t_ms < candidate_since_ms_) {
            resetCandidate();
        }
        if (!candidate_pending_) {
            candidate_pending_ = true;
            candidate_since_ms_ = input.t_ms;
        }
        if (input.t_ms - candidate_since_ms_ >= dwellFor(candidate)) {
            state_ = candidate;
            resetCandidate();
        }
    }

    // Confidence is the evidence's support for the COMMITTED state (D5):
    // an in-band Worn reports the sagging score honestly rather than 1.
    float confidence = 0.0f;
    if (state_ == WearState::Worn) {
        confidence = combinedScore(evidence);
    } else if (state_ == WearState::NotWorn) {
        confidence = 1.0f - combinedScore(evidence);
    }
    // §9: without contact evidence the IMU alone never claims to be sure.
    if (!evidence.ppg_available && confidence > kImuOnlyMaxConfidence) {
        confidence = kImuOnlyMaxConfidence;
    }

    return WearEstimate{input.t_ms, state_, confidence};
}

float WearDetector::combinedScore(const WearEvidence& evidence) const {
    if (evidence.ppg_available && evidence.imu_available) {
        return kWearPpgWeight * evidence.ppg_score + kWearImuWeight * evidence.imu_score;
    }
    return evidence.ppg_available ? evidence.ppg_score : evidence.imu_score;
}

WearState WearDetector::candidateState(float score) const {
    if (score >= config_.worn_score_enter) {
        return WearState::Worn;
    }
    if (score <= config_.worn_score_exit) {
        return WearState::NotWorn;
    }
    return state_;  // hysteresis band: hold, so Uncertain never flips here
}

TimeMs WearDetector::dwellFor(WearState candidate) const {
    if (candidate == WearState::Worn) {
        return config_.worn_confirm_ms;
    }
    if (candidate == WearState::NotWorn) {
        return config_.not_worn_confirm_ms;
    }
    return 0;  // unreachable via candidateState(); keeps the switch total
}

void WearDetector::resetCandidate() {
    candidate_pending_ = false;
    candidate_since_ms_ = 0;
}

}  // namespace bw::wear
