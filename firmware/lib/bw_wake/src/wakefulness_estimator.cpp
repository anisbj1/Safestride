// bw/wake/wakefulness_estimator.cpp - epoch rolling + demo fast lane.

#include <bw/wake/wakefulness_estimator.h>

#include <cmath>

namespace bw::wake {

namespace {

WakeInputRejection inputRejection(const WakeSample& sample, TimeMs previous_sample_ms,
                                  const WearEstimate* wear) {
    if (wear != nullptr) {
        if (wear->state != WearState::Worn)
            return WakeInputRejection::WearUnsafe;
        if (sample.t_ms < wear->t_ms)
            return WakeInputRejection::WearFuture;
        if (sample.t_ms - wear->t_ms > kDemoWearFreshnessMs)
            return WakeInputRejection::WearStale;
    }
    if (previous_sample_ms != 0 && sample.t_ms <= previous_sample_ms)
        return WakeInputRejection::SampleNotIncreasing;
    if (sample.t_ms < sample.motion.t_ms)
        return WakeInputRejection::MotionFuture;
    if (sample.t_ms - sample.motion.t_ms > kDemoMotionFreshnessMs)
        return WakeInputRejection::MotionStale;
    if (sample.quality.imu == DataQuality::Insufficient)
        return WakeInputRejection::ImuInsufficient;
    if (sample.motion.quality == DataQuality::Insufficient)
        return WakeInputRejection::MotionInsufficient;
    if (!std::isfinite(sample.motion.enmo_g) || sample.motion.enmo_g < 0.0f)
        return WakeInputRejection::EnmoInvalid;
    if (previous_sample_ms != 0 && sample.t_ms - previous_sample_ms > kDemoInputGapMs)
        return WakeInputRejection::CadenceGap;
    return WakeInputRejection::None;
}

}  // namespace

WakefulnessEstimator::WakefulnessEstimator(const WakeConfig& cfg, const IClock* clock,
                                           const IBaselineProvider* baseline, IModel* model)
    : cfg_(cfg), model_(model), assembler_(cfg, clock, baseline), heuristic_(cfg),
      model_estimator_(model, cfg), last_{}, fast_lane_{}, last_sample_ms_(0),
      last_wake_check_ms_(0), trigger_armed_(false), hold_awake_(false),
      first_still_pending_(false) {}

WakeEstimate WakefulnessEstimator::step(const WakeSample& sample, bool wake_check_has_value,
                                        TimeMs wake_check_passed_ms, const WearEstimate* wear) {
    const WakeInputRejection rejection = inputRejection(sample, last_sample_ms_, wear);
    last_input_rejection_ = rejection;
    if (rejection == WakeInputRejection::WearUnsafe ||
        rejection == WakeInputRejection::WearFuture || rejection == WakeInputRejection::WearStale) {
        reset();
        last_input_rejection_ = rejection;
        // Consume an old response so rewear cannot replay a previous check.
        last_wake_check_ms_ = wake_check_has_value ? wake_check_passed_ms : 0;
        WakeEstimate unavailable{};
        unavailable.probs.t_ms = sample.t_ms;
        unavailable.probs.input_quality = DataQuality::Insufficient;
        return unavailable;
    }
    const bool check_is_new = wake_check_has_value && wake_check_passed_ms != 0 &&
                              wake_check_passed_ms != last_wake_check_ms_ &&
                              sample.t_ms >= wake_check_passed_ms &&
                              sample.t_ms - wake_check_passed_ms <= kDemoResponseFreshnessMs;
    if (check_is_new) {
        last_wake_check_ms_ = wake_check_passed_ms;
        trigger_armed_ = false;
        first_still_pending_ = false;
        hold_awake_ = true;
        awake_evidence_ms_ = wake_check_passed_ms;
    }

    const bool increasing = last_sample_ms_ == 0 || sample.t_ms > last_sample_ms_;
    const bool usable =
        rejection == WakeInputRejection::None || rejection == WakeInputRejection::CadenceGap;
    const bool gap_ok = rejection != WakeInputRejection::CadenceGap;
    if (increasing)
        last_sample_ms_ = sample.t_ms;

    bool fast_updated = false;
    if (wear != nullptr && !have_contact_ && usable && gap_ok) {
        have_contact_ = true;
        initial_contact_ms_ = sample.t_ms;
    }
    if (hold_awake_ && (sample.t_ms < awake_evidence_ms_ ||
                        sample.t_ms - awake_evidence_ms_ >= kDemoAwakeEvidenceMs)) {
        hold_awake_ = false;
    }
    if (usable && gap_ok) {
        const bool strong_burst = sample.motion.enmo_g >= kDemoStrongMotionEnmoG;
        if (strong_burst) {
            trigger_armed_ = true;
            first_still_pending_ = true;
            hold_awake_ = false;
        }
        if (sample.motion.enmo_g > cfg_.demo_awake_motion_enmo_g) {
            // Strong movement arms a subsequent stillness trigger. Ordinary
            // gentle movement cancels it and supplies bounded recent Awake evidence.
            if (!strong_burst) {
                trigger_armed_ = false;
                first_still_pending_ = false;
                hold_awake_ = true;
                awake_evidence_ms_ = sample.t_ms;
            }
            fast_lane_ = demoEstimate(sample.t_ms, true, sample.quality.imu, strong_burst);
            fast_updated = true;
        } else if (trigger_armed_ && !hold_awake_) {
            fast_lane_ = demoEstimate(sample.t_ms, false, sample.quality.imu, first_still_pending_);
            first_still_pending_ = false;
            fast_updated = true;
        } else if (hold_awake_) {
            fast_lane_ = demoEstimate(sample.t_ms, true, sample.quality.imu, false);
            fast_updated = true;
        } else if (have_contact_ && sample.t_ms >= initial_contact_ms_ &&
                   sample.t_ms - initial_contact_ms_ < cfg_.epoch_length_ms) {
            // Initial contact bridges only the first epoch, never an indefinite
            // wake claim. F10 still asks at 20 s, including during this grace.
            fast_lane_ = demoEstimate(sample.t_ms, true, sample.quality.imu, false);
            fast_updated = true;
        }
    } else {
        // Bad or discontinuous data breaks the armed stillness cycle; no
        // trigger may bridge an IMU gap or use stale data as evidence.
        trigger_armed_ = false;
        first_still_pending_ = false;
        hold_awake_ = false;
    }

    EpochSummary epoch{};
    const bool epoch_ready = assembler_.step(sample, epoch);
    // Keep the background epoch current even while recent evidence wins.
    // Otherwise expiring the bounded contact grace could expose an empty
    // answer for an entire additional epoch.
    if (epoch_ready) {
        WakeEstimate estimate{};
        if (model_ != nullptr)
            estimate = model_estimator_.estimate(epoch, assembler_.history());
        if (!estimate.probs.valid)
            estimate = heuristic_.estimate(epoch, assembler_.history());
        last_ = estimate;
    }
    if (!usable || !gap_ok) {
        WakeEstimate unavailable{};
        unavailable.probs.t_ms = sample.t_ms;
        unavailable.probs.valid = false;
        unavailable.probs.input_quality = DataQuality::Insufficient;
        return unavailable;
    }
    WakeEstimate result = (fast_updated || trigger_armed_ || hold_awake_) ? fast_lane_ : last_;
    // CR-034: the evidence belongs to THIS processed window, even when the
    // probabilities are an older epoch or a held Awake answer.
    result.probs.wake_motion = {sample.motion.t_ms, true,
                                sample.motion.enmo_g > cfg_.demo_awake_motion_enmo_g};
    return result;
}

WakeEstimate WakefulnessEstimator::demoEstimate(TimeMs t_ms, bool awake, DataQuality quality,
                                                bool short_motion_burst) const {
    WakeEstimate result{};
    result.probs.t_ms = t_ms;
    result.probs.valid = true;
    result.probs.awake = awake ? kDemoAwakeProbability : kDemoDrowsyAwakeProbability;
    result.probs.drowsy = awake ? kDemoAwakeDrowsyProbability : kDemoDrowsyProbability;
    result.probs.sleep = awake ? kDemoAwakeSleepProbability : kDemoDrowsySleepProbability;
    const float quality_factor =
        quality == DataQuality::Good ? kConfidenceGoodFactor : kConfidenceDegradedFactor;
    result.probs.confidence = quality_factor * kDemoImuOnlyConfidence;
    result.probs.input_quality = quality;
    if (!awake) {
        setReason(result.reasons, ReasonCode::SustainedLowMotion);
    }
    if (short_motion_burst) {
        setReason(result.reasons, ReasonCode::ShortMotionBurst);
    }
    result.probs.reasons = result.reasons;
    return result;
}

void WakefulnessEstimator::reset() {
    assembler_.reset();
    last_ = WakeEstimate{};
    fast_lane_ = WakeEstimate{};
    last_sample_ms_ = 0;
    last_wake_check_ms_ = 0;
    awake_evidence_ms_ = 0;
    initial_contact_ms_ = 0;
    have_contact_ = false;
    trigger_armed_ = false;
    hold_awake_ = false;
    first_still_pending_ = false;
    last_input_rejection_ = WakeInputRejection::None;
}

}  // namespace bw::wake
