#include <bw/activity/activity_classifier.h>

namespace bw::activity {

void ActivityClassifier::noteTimeAndMaybeResetDwell(TimeMs t_ms) {
    const bool jumped =
        have_prev_t_ && (t_ms < prev_t_ms_ || t_ms - prev_t_ms_ > config_.max_feature_step_gap_ms);
    if (jumped) {
        // D4: dwell counts *continuous* evidence. After a lost-window gap or
        // a backwards step the counter restarts with the candidate cleared,
        // so pre-gap accumulation can never complete a transition.
        dwell_windows_ = 0;
        candidate_ = current_;
        candidate_confidence_ = current_confidence_;
    }
    prev_t_ms_ = t_ms;
    have_prev_t_ = true;
}

ActivityEstimate ActivityClassifier::step(const MotionFeatures& features) {
    noteTimeAndMaybeResetDwell(features.t_ms);

    // Cadence advances every window; an Insufficient window zeroes
    // dominant_freq_hz upstream (02 §4.2), which forces CadenceState::None.
    const CadenceState cadence = cadence_.step(features.dominant_freq_hz, features.enmo_g);

    // R1 — no reliable data: commit Unknown immediately (spec §10). The
    // stale label must not outlive the data that supported it, so this is
    // the one transition that bypasses the dwell (D4).
    if (features.quality == DataQuality::Insufficient) {
        current_ = ActivityContext::Unknown;
        current_confidence_ = 0.0f;
        candidate_ = ActivityContext::Unknown;
        candidate_confidence_ = 0.0f;
        dwell_windows_ = 0;
        return estimate(features.t_ms);
    }

    const RawClassification raw = classify(features, cadence);

    if (raw.context == current_) {
        // Same context: refresh confidence in place (tracks quality changes)
        // and drop any pending candidate.
        current_confidence_ = raw.confidence;
        candidate_ = raw.context;
        candidate_confidence_ = raw.confidence;
        dwell_windows_ = 0;
        return estimate(features.t_ms);
    }

    if (raw.context == candidate_) {
        ++dwell_windows_;  // candidate confirmed again; never exceeds
                           // min_dwell_windows (commit resets below)
        // D5: the confidence committed is the *latest* one, not the one
        // captured when the candidate first appeared — a quality drop
        // mid-dwell must lower the committed confidence too.
        candidate_confidence_ = raw.confidence;
    } else {
        candidate_ = raw.context;
        candidate_confidence_ = raw.confidence;
        dwell_windows_ = 1;
    }

    if (dwell_windows_ >= config_.min_dwell_windows) {
        current_ = candidate_;
        current_confidence_ = candidate_confidence_;
        dwell_windows_ = 0;
    }
    return estimate(features.t_ms);
}

ActivityClassifier::RawClassification ActivityClassifier::classify(const MotionFeatures& f,
                                                                   CadenceState cadence) const {
    const ActivityConfig& c = config_;
    RawClassification raw{ActivityContext::Unknown, 0.0f};

    if (f.stillness_ms >= c.prolonged_inactivity_ms) {
        // R2 — duration evidence beats posture/cadence labels; explicitly
        // not sleep (F03 §5: low movement never implies sleep).
        raw = {ActivityContext::InactiveProlonged, c.conf_inactive_prolonged};
    } else if (cadence == CadenceState::Running) {
        // R3 — narrow cadence band + impact energy (two agreeing cues).
        raw = {ActivityContext::Running, c.conf_running};
    } else if (cadence == CadenceState::Walking) {
        // R4 — step band + whole-body motion (tapping is rejected by the
        // ENMO gate inside CadenceDetector, F03 D3).
        raw = {ActivityContext::Walking, c.conf_walking};
    } else if (f.stillness_ms > 0u || f.enmo_g < c.stationary_enmo_max_g) {
        // `still`: F02's stillness state (Good windows) or the ENMO floor
        // (keeps Degraded windows classifiable — F02 zeroes stillness_ms
        // unless quality is Good).
        if (f.gravity_tilt_deg >= c.lying_tilt_min_deg &&
            f.gravity_tilt_deg <= c.lying_tilt_max_deg) {
            // R5 — still + near-horizontal: consistent with a resting
            // posture, capped at the reporting floor (D2/D5).
            raw = {ActivityContext::LyingResting, c.posture_confidence_cap};
        } else {
            // R6 — no meaningful motion; Sitting/Standing collapse here
            // because wrist tilt cannot separate them (D6, 02 §4.3).
            raw = {ActivityContext::Stationary, c.conf_stationary};
        }
    } else {
        // R7 — moving, no cadence row matched (slow gait, chores,
        // gestures): honest Unknown beats force-fitting (00 §7).
        raw = {ActivityContext::Unknown, 0.0f};
    }

    // R9 — degraded IMU data lowers every claim by a quarter; posture at
    // the floor drops out first, robust rows stay reportable (D5).
    if (raw.context != ActivityContext::Unknown && f.quality == DataQuality::Degraded) {
        raw.confidence *= c.degraded_confidence_factor;
    }

    // R8 — reporting floor: below it we report no classification at all,
    // with confidence 0.0 (no claim made).
    if (raw.context != ActivityContext::Unknown && raw.confidence < c.min_confidence_to_report) {
        return RawClassification{ActivityContext::Unknown, 0.0f};
    }
    return raw;
}

}  // namespace bw::activity
