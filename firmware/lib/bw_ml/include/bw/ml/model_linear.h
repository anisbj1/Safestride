#pragma once

// bw/ml/model_linear.h — LinearSoftmaxModel (F05 D7): max-shifted softmax over
// row-major weights. Pure math on the training-space domain (D4): the caller's
// FeatureVector::v[] already holds normalized values, this class knows nothing
// about sensors, states or alerts.

#include <bw/model.h>
#include <bw/result.h>

#include <bw/ml/model_descriptor.h>

namespace bw::ml {

// Constant-array payload emitted by export_cpp.py into model_<name>_v<N>.h.
// weights is row-major [n_classes][feature_count]; bias is [n_classes].
struct LinearModelData {
    const float* weights;
    const float* bias;
    uint8_t feature_count;
    ModelDescriptor descriptor;
};

class LinearSoftmaxModel final : public IModel {
public:
    // References generated constants: construction is free, storage is the
    // header's (static, flash-backed); the model must outlive this object.
    explicit LinearSoftmaxModel(const LinearModelData& data) noexcept : data_(data) {}

    uint16_t schemaId() const noexcept override { return data_.descriptor.schema_id; }

    Result<void> predict(const FeatureVector&, ModelOutput&) noexcept override;
    Result<size_t> contributions(const FeatureVector&, Contribution* out,
                                 size_t max) noexcept override;

    // App-readable metadata (F05 §7: concrete type, not on IModel — §13).
    const ModelDescriptor& descriptor() const noexcept { return data_.descriptor; }
    const LinearModelData& data() const noexcept { return data_; }

private:
    const LinearModelData& data_;
};

}  // namespace bw::ml
