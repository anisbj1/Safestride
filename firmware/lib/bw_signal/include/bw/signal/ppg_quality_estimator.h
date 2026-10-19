#pragma once

// bw/signal/ppg_quality_estimator.h — F02 §7: SQI + DataQuality for one
// PpgFeatures frame (F02 D6).
//
// sqi = perfusion factor x timing factor x saturation factor, each 0..1
// (product: weight-free and explainable; a weighted sum would need tuning
// data we do not have yet). Quality is the WORST rule that fires:
//   Insufficient — no samples, coverage < kMinWindowCoverageFrac,
//                  saturation >= kMaxSaturatedFraction, DC <= 0,
//                  sqi < kMinSqiForValidHr  (§9: noise-only/saturated land here)
//   Degraded     — gap > kMaxSampleGapMs or a stream anomaly (§10)
//   Good         — otherwise
// motion_corrupted is applied by SignalProcessor (it needs IMU data).

#include <cmath>

#include <bw/hardware.h>
#include <bw/quality.h>
#include <bw/signal/signal_config.h>
#include <bw/types.h>

namespace bw::signal {

// Per-frame PPG accumulators (F02 D6). accumulate() runs per sample during
// the drain; finalize() derives the ratios consumed by evaluate().
struct PpgFrameStats {
    uint32_t sample_count = 0;
    uint32_t saturated_count = 0;
    TimeMs max_gap_ms = 0;
    float sum_raw_ir = 0.0f;
    float sum_filtered_sq = 0.0f;
    float sum_filtered = 0.0f;  // FIX-2 observability: for the in-window std
    bool anomaly = false;       // backwards timestamp or staging overflow
    // Raw IR span over the frame (bench observability, F02 FIX-1): min == max
    // == full scale = ADC railed (flatline); max - min ~ pulse amplitude in
    // counts. Valid only when sample_count > 0 (reset() leaves 0/0).
    uint32_t raw_ir_min = 0;
    uint32_t raw_ir_max = 0;

    // Derived (filled by finalize()).
    float dc_level = 0.0f;
    float ac_rms = 0.0f;
    float perfusion_index = 0.0f;
    float saturated_fraction = 0.0f;
    float coverage = 0.0f;
    // FIX-2 observability ([PPGPK] line): std of the filtered stream over
    // the window [DERIVED] — the in-band noise/drift level a pulse must
    // stand out of. 0 when the window has no samples.
    float filtered_std = 0.0f;

    void reset() { *this = PpgFrameStats{}; }

    void accumulate(const PpgSample& sample, float filtered, TimeMs gap_ms) {
        if (gap_ms > max_gap_ms) {
            max_gap_ms = gap_ms;
        }
        ++sample_count;
        if (sample_count == 1u || sample.ir < raw_ir_min) {
            raw_ir_min = sample.ir;
        }
        if (sample_count == 1u || sample.ir > raw_ir_max) {
            raw_ir_max = sample.ir;
        }
        if (sample.saturated) {
            ++saturated_count;
        }
        sum_raw_ir += static_cast<float>(sample.ir);
        sum_filtered += filtered;
        sum_filtered_sq += filtered * filtered;
    }

    void finalize(TimeMs window_ms, uint32_t sample_period_ms = kPpgSamplePeriodMs) {
        if (sample_count == 0) {
            dc_level = 0.0f;
            ac_rms = 0.0f;
            perfusion_index = 0.0f;
            filtered_std = 0.0f;
        } else {
            const float n = static_cast<float>(sample_count);
            dc_level = sum_raw_ir / n;
            ac_rms = std::sqrt(sum_filtered_sq / n);
            perfusion_index = dc_level > 0.0f ? ac_rms / dc_level : 0.0f;
            // FIX-2 observability: population std of the filtered stream.
            // The max(0) guard absorbs float cancellation (var is a
            // difference of two ~equal positives at tiny spread).
            const float mean_f = sum_filtered / n;
            const float var = sum_filtered_sq / n - mean_f * mean_f;
            filtered_std = var > 0.0f ? std::sqrt(var) : 0.0f;
        }
        saturated_fraction = sample_count == 0 ? 0.0f
                                               : static_cast<float>(saturated_count) /
                                                     static_cast<float>(sample_count);
        coverage = static_cast<float>(sample_count) * static_cast<float>(sample_period_ms) /
                   static_cast<float>(window_ms);
    }
};

struct PpgQualityResult {
    DataQuality quality;
    float sqi;  // 0..1 (02 §4.2)
};

class PpgQualityEstimator {
public:
    // ibi_consistency comes from HeartRateEstimator (0 when there is no IBI
    // yet — a window without consistent timing cannot be Good, F02 D6).
    static PpgQualityResult evaluate(const PpgFrameStats& stats, float ibi_consistency);
};

}  // namespace bw::signal
