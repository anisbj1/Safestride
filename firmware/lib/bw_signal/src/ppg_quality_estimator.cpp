#include <bw/signal/ppg_quality_estimator.h>

namespace bw::signal {

PpgQualityResult PpgQualityEstimator::evaluate(const PpgFrameStats& stats, float ibi_consistency) {
    // Factors, each 0..1 with a stated derivation (F02 D6).
    float pi_factor = 0.0f;
    if (stats.perfusion_index > kMinPerfusionIndex) {
        pi_factor = (stats.perfusion_index - kMinPerfusionIndex) /
                    (kSqiFullPerfusionRef - kMinPerfusionIndex);
        if (pi_factor > 1.0f) {
            pi_factor = 1.0f;
        }
    }
    if (ibi_consistency < 0.0f || ibi_consistency > 1.0f) {
        ibi_consistency = 0.0f;  // defensive: clamp an out-of-range input
    }
    const float timing_factor = ibi_consistency;
    const float saturation_factor = stats.saturated_fraction < kMaxSaturatedFraction ? 1.0f : 0.0f;

    const float sqi = pi_factor * timing_factor * saturation_factor;

    // Worst rule wins (F02 D6); "no reliable data" beats a guess (00 §1).
    PpgQualityResult out;
    if (stats.sample_count == 0 || stats.coverage < kMinWindowCoverageFrac ||
        stats.saturated_fraction >= kMaxSaturatedFraction || stats.dc_level <= 0.0f ||
        sqi < kMinSqiForValidHr) {
        out.quality = DataQuality::Insufficient;
    } else if (stats.max_gap_ms > kMaxSampleGapMs || stats.anomaly) {
        out.quality = DataQuality::Degraded;
    } else {
        out.quality = DataQuality::Good;
    }
    out.sqi = sqi;
    return out;
}

}  // namespace bw::signal
