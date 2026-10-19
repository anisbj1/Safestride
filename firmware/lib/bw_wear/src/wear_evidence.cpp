#include <bw/wear/wear_evidence.h>

#include <bw/quality.h>
#include <bw/wear/wear_config.h>

namespace bw::wear {
namespace {

// Clamps to [0,1] such that NaN also lands at 0: NaN fails every `>`, so it
// falls through both guards — garbage numbers must never become a score.
float clamp01(float value) {
    if (value > 1.0f) {
        return 1.0f;
    }
    if (value > 0.0f) {
        return value;
    }
    return 0.0f;
}

// Fresh = stamped no later than now and within kFeatureStaleMs; a future
// stamp means the clock pairing is wrong, so the value counts as absent.
bool isFresh(TimeMs t_ms, TimeMs now_ms) {
    return t_ms <= now_ms && (now_ms - t_ms) <= kFeatureStaleMs;
}

// Contact: ambient-leak floor -> solid-contact plateau, linear between.
float contactScore(const PpgFeatures& ppg) {
    return clamp01((ppg.ir_dc_level - kPpgContactIrDcMin) /
                   (kPpgContactIrDcFull - kPpgContactIrDcMin));
}

// Pulsatility: noise floor -> firm-contact plateau, linear between.
float perfusionScore(const PpgFeatures& ppg) {
    return clamp01((ppg.perfusion_index - kPpgPerfusionMin) /
                   (kPpgPerfusionFull - kPpgPerfusionMin));
}

// Micro-motion: desk floor -> carried-by-a-human plateau, linear between.
float microMotionScore(const MotionFeatures& motion) {
    return clamp01((motion.enmo_g - kDeskMotionEnmoMaxG) /
                   (kMicroMotionEnmoFullG - kDeskMotionEnmoMaxG));
}

}  // namespace

WearEvidence evaluateWearEvidence(const PpgFeatures* ppg, const MotionFeatures* motion,
                                  TimeMs now_ms) {
    WearEvidence evidence;

    // PPG availability (D3): a computed DC proves the window ran; `dc > 0`
    // is F02's own "no samples" marker (F02 zero-fills ONLY an empty window,
    // sample_count == 0; a partial-coverage or Degraded window carries its
    // real DC — which D3 deliberately keeps as contact evidence even when F02
    // called the window unusable).
    // Quality is deliberately not a gate - Insufficient-perfusion still
    // yields valid contact readings.
    if (ppg != nullptr && isFresh(ppg->t_ms, now_ms) && ppg->ir_dc_level > 0.0f) {
        evidence.ppg_available = true;
        // F02 may expose plausible-looking HR/SQI values in an Insufficient
        // window. Those quality-dependent terms cannot support wear then;
        // computed DC/perfusion remain contact evidence per D3.
        const bool contact_present = ppg->ir_dc_level > kPpgContactIrDcMin;
        const bool ppg_quality_sufficient = ppg->quality != DataQuality::Insufficient;
        // Relative AC/DC can be large for tiny ambient readings (bench: DC
        // ~450, PI ~0.07 with no finger). Without absolute contact, perfusion,
        // HR and SQI are not independent positive evidence of wearing.
        evidence.ppg_score =
            contact_present
                ? kPpgContactWeight * contactScore(*ppg) +
                      kPpgPerfusionWeight * perfusionScore(*ppg) +
                      (ppg_quality_sufficient ? kPpgHrWeight * (ppg->hr_valid ? 1.0f : 0.0f) +
                                                    kPpgSqiWeight * clamp01(ppg->sqi)
                                              : 0.0f)
                : 0.0f;
    }

    // IMU availability (D3): below Insufficient the window statistics are
    // undefined (F02 coverage rule), so there is nothing to score.
    if (motion != nullptr && isFresh(motion->t_ms, now_ms) &&
        motion->quality != DataQuality::Insufficient) {
        evidence.imu_available = true;
        const float stillness = motion->stillness_ms >= kDeskStillnessMs ? 0.0f : 1.0f;
        evidence.imu_score =
            kImuMicroMotionWeight * microMotionScore(*motion) + kImuStillnessWeight * stillness;
    }

    return evidence;
}

}  // namespace bw::wear
