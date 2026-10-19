#pragma once

// bw/model.h — docs/02_INTERFACES.md §4.4 (ML layer: F05 owns IModel, F06/F09 consume).
// Probabilities are float in [0,1]; "not available" is a validity flag, never a
// magic number (02 §1).

#include <cstddef>
#include <cstdint>

#include <bw/quality.h>
#include <bw/result.h>
#include <bw/state.h>  // ReasonMask (CR-010, accepted 2026-10-11)
#include <bw/types.h>

namespace bw {

// [EXPERIMENTAL] Static upper bound on feature-vector length (F00 §8).
// Why: fixed storage for FeatureVector::v; raise via CR only if a real schema needs
// more than 32 features (today's alertness schema is far smaller).
constexpr size_t kMaxFeatures = 32;

// [EXPERIMENTAL] Names the otherwise-bare `float p[8]` of 02 §4.4 (naming it is
// syntax, allowed by the DRAFT note). Why 8: today's problems are 3-class alertness
// and 2-class detectors; 8 is headroom for a binary event detector plus Unknown.
constexpr uint8_t kMaxModelClasses = 8;

struct FeatureVector {
    uint16_t schema_id;
    uint16_t schema_version;
    uint8_t count;
    float v[kMaxFeatures];
    TimeMs t_ms;
};

// Interpretable per-class probabilities: p[0..n_classes-1], n_classes <= kMaxModelClasses.
struct ModelOutput {
    uint8_t n_classes;
    float p[kMaxModelClasses];
    float confidence;  // 0..1, NOT clinical certainty
};

// Contribution of one feature to the current prediction (02 §7, defined by F00).
struct Contribution {
    uint8_t feature_index;
    float weighted_value;
};

class IModel {
public:
    virtual uint16_t schemaId() const = 0;
    // Deterministic, no allocation (02 §4.4): the assembler's schema_id/version must
    // match schemaId() or predict() fails (02 §6.5).
    virtual Result<void> predict(const FeatureVector&, ModelOutput&) = 0;
    // Optional explainability; implementations may return Err::Unsupported (F22).
    virtual Result<size_t> contributions(const FeatureVector&, Contribution* out, size_t max) = 0;
};

struct AlertnessProbabilities {
    TimeMs t_ms;
    bool valid;  // false = no estimate this epoch (02 §1 validity rule)
    float awake;
    float drowsy;
    float sleep;
    float confidence;
    DataQuality input_quality;
    // CR-010 (accepted 2026-10-11, integrator G0): evidence F06 saw that supports
    // this estimate — bits set only for evidence the estimator actually weighed
    // (00 §1: never fabricate). Empty mask = "probabilities only, no reason code
    // claimed". Not persisted or transmitted; F07 decides whether to merge these
    // bits into StateSnapshot.reasons. Trailing field on an in-memory struct.
    ReasonMask reasons;
    WakeMotionEvidence wake_motion;  // CR-034: original window stamp, never held evidence
};

}  // namespace bw
