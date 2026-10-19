#pragma once

// bw/ml/model_descriptor.h — F05 spec §7: embedded model metadata (D2).
//
// A concrete, app-readable type generated into each model_<name>_v<N>.h. It is
// deliberately NOT part of IModel: exposing metadata to features would be a
// contract change (F05 §13 [ASSUMED]); src/app and the wiring layer read it
// directly from the generated header.

#include <cstdint>

namespace bw::ml {

// Everything needed to identify what a model was trained on. Field order is
// fixed by F05 spec §7; generated headers use aggregate initialization.
struct ModelDescriptor {
    const char* name;                // model name, snake_case (matches the header namespace)
    uint32_t model_version;          // retraining bumps this, not schema_version (§2.3)
    uint16_t schema_id;              // FeatureVector must carry exactly this id...
    uint16_t schema_version;         // ...and this version (the §3 handshake)
    uint8_t n_classes;               // 1..bw::kMaxModelClasses; 0 = degenerate -> Err::Corrupt
    const char* const* class_names;  // n_classes entries; index = ModelOutput::p position
    const char* training_data_hash;  // provenance string from the exporter (D2/D8)
};

}  // namespace bw::ml
