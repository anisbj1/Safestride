#pragma once

// bw/ml/model_validate.h — shared runtime handshake for both model families
// (F05 D3: predict()/contributions() validate schema_id, schema_version and
// count BEFORE touching FeatureVector::v[]; mismatches are Err::InvalidArg,
// degenerate embedded metadata is Err::Corrupt — FEATURE_SCHEMA.md §3).
//
// Internal helper header, not a contract: only bw_ml sources include it.

#include <cmath>

#include <bw/model.h>
#include <bw/result.h>

#include <bw/ml/model_descriptor.h>

namespace bw::ml {

// Embedded metadata sanity. Runs first so a corrupt header can never be
// mistaken for caller error (and never reads v[] at all).
inline Result<void> validateDescriptor(const ModelDescriptor& descriptor,
                                       uint8_t feature_count) noexcept {
    if (descriptor.schema_id == 0 || descriptor.schema_version == 0) {
        return {Err::Corrupt};  // id/version 0 are never issued (FEATURE_SCHEMA.md §2.4)
    }
    if (descriptor.n_classes == 0 || descriptor.n_classes > kMaxModelClasses) {
        return {Err::Corrupt};  // degenerate metadata (§3), also bounds ModelOutput::p
    }
    if (feature_count == 0 || feature_count > kMaxFeatures) {
        return {Err::Corrupt};  // would read past FeatureVector::v[] otherwise
    }
    if (descriptor.class_names == nullptr) {
        return {Err::Corrupt};
    }
    return {Err::Ok};
}

// Caller-supplied vector: handshake (both ids + count) first, then value
// finiteness. The count check precedes the v[] scan by construction, so a
// count of 255 can never index out of bounds (D3).
inline Result<void> validateVector(const FeatureVector& vector, const ModelDescriptor& descriptor,
                                   uint8_t feature_count) noexcept {
    if (vector.schema_id != descriptor.schema_id ||
        vector.schema_version != descriptor.schema_version) {
        return {Err::InvalidArg};
    }
    if (vector.count != feature_count) {
        return {Err::InvalidArg};
    }
    for (uint8_t i = 0; i < vector.count; ++i) {
        if (!std::isfinite(vector.v[i])) {
            return {Err::InvalidArg};  // never propagate NaN/Inf (§3)
        }
    }
    return {Err::Ok};
}

inline Result<void> validateInput(const FeatureVector& vector, const ModelDescriptor& descriptor,
                                  uint8_t feature_count) noexcept {
    Result<void> result = validateDescriptor(descriptor, feature_count);
    if (!result.ok()) {
        return result;
    }
    return validateVector(vector, descriptor, feature_count);
}

}  // namespace bw::ml
