#pragma once

// bw/wear/wear_evidence.h — F04 §7: per-signal evidence scoring (D3/D4).
//
// A source is "available" when it actually produced computed values for the
// current window — not when its DataQuality is Good (D3): F02 marks PPG
// Insufficient for low perfusion, and that low contact is exactly the
// not-worn evidence this feature must keep. Quality still shapes the score
// through sqi / hr_valid.
//
// Pure function, no state, no platform calls: host-testable (01 §1 rule 5).

#include <bw/features.h>
#include <bw/types.h>

namespace bw::wear {

struct WearEvidence {
    bool ppg_available = false;
    float ppg_score = 0.0f;  // 0..1, meaningful only when ppg_available
    bool imu_available = false;
    float imu_score = 0.0f;  // 0..1, meaningful only when imu_available
};

// Scores both sources. nullptr = source absent; a feature whose t_ms is
// older than kFeatureStaleMs relative to now_ms (or stamped in the future)
// counts as absent. Non-finite inputs are clamped to 0 / treated as absent,
// never propagated (00 §7.5: no fabricated confidence).
WearEvidence evaluateWearEvidence(const PpgFeatures* ppg, const MotionFeatures* motion,
                                  TimeMs now_ms);

}  // namespace bw::wear
