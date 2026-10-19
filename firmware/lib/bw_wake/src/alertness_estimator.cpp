// bw/wake/alertness_estimator.cpp — shared estimator helpers.

#include <bw/wake/alertness_estimator.h>

#include <bw/wake/wake_math.h>

namespace bw::wake {

float confidenceCap(const EpochSummary& e, const WakeConfig& cfg) {
    if (!e.imu_usable) {
        return 0.0f;  // no IMU evidence: nothing may be claimed (F06 §5)
    }

    float confidence = cfg.base_confidence;
    confidence *= (e.quality == DataQuality::Good) ? cfg.confidence_good_factor
                                                   : cfg.confidence_degraded_factor;

    // Completeness: a half-empty epoch is weaker evidence than a full one.
    const float expected = static_cast<float>(expectedSamplesPerEpoch(cfg));
    const float fill = expected > 0.0f ? static_cast<float>(e.sample_count) / expected : 0.0f;
    confidence *= clamp01(fill);

    // Each missing optional source costs its own factor, so a lossy epoch
    // says so in one number instead of pretending the gaps were not there.
    if (!e.ppg_usable) {
        confidence *= cfg.confidence_ppg_missing_factor;
    }
    if (!e.baseline_valid) {
        confidence *= cfg.confidence_missing_optional_factor;
    }
    if (!e.tod_valid) {
        confidence *= cfg.confidence_missing_optional_factor;
    }
    return clamp01(confidence);
}

ReasonMask qualityReasons(const EpochSummary& e) {
    ReasonMask reasons = 0;
    if (e.quality != DataQuality::Good) {
        setReason(reasons, ReasonCode::LowSignalQuality);
    }
    if (!e.imu_usable || !e.ppg_usable) {
        setReason(reasons, ReasonCode::SensorUnavailable);
    }
    return reasons;
}

}  // namespace bw::wake
