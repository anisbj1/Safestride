#include <bw/ml/model_linear.h>

#include <cmath>

#include <bw/ml/model_validate.h>

namespace bw::ml {
namespace {

// First-index argmax: the tie rule both model families use (deterministic).
uint8_t argmaxOf(const float* values, uint8_t count) noexcept {
    uint8_t best = 0;
    for (uint8_t i = 1; i < count; ++i) {
        if (values[i] > values[best]) {
            best = i;
        }
    }
    return best;
}

}  // namespace

Result<void> LinearSoftmaxModel::predict(const FeatureVector& vector, ModelOutput& out) noexcept {
    const Result<void> check = validateInput(vector, data_.descriptor, data_.feature_count);
    if (!check.ok()) {
        return check;
    }

    const uint8_t n_classes = data_.descriptor.n_classes;
    float logits[kMaxModelClasses] = {};
    for (uint8_t c = 0; c < n_classes; ++c) {
        const float* row = data_.weights + static_cast<size_t>(c) * data_.feature_count;
        float sum = data_.bias[c];
        for (uint8_t i = 0; i < data_.feature_count; ++i) {
            sum += row[i] * vector.v[i];
        }
        // Max-shift cannot repair an already-overflowed dot product: reachable
        // only with corrupt weights or a wildly out-of-domain input, so refuse
        // (Err::Corrupt, FEATURE_SCHEMA.md §3) instead of letting NaN out.
        if (!std::isfinite(sum)) {
            return {Err::Corrupt};
        }
        logits[c] = sum;
    }

    // Max-shift softmax (D7): exp(z - max z) is in (0, 1] so large finite
    // logits cannot overflow, and the term for the max logit is exactly 1,
    // which keeps the sum >= 1 (no division by zero).
    float top = logits[0];
    for (uint8_t c = 1; c < n_classes; ++c) {
        if (logits[c] > top) {
            top = logits[c];
        }
    }
    float exps[kMaxModelClasses] = {};
    float exp_sum = 0.0f;
    for (uint8_t c = 0; c < n_classes; ++c) {
        exps[c] = std::exp(logits[c] - top);
        exp_sum += exps[c];
    }

    // Assemble locally and assign only on success: on any Err above, the
    // caller's output argument is left untouched (§3).
    ModelOutput local{};
    local.n_classes = n_classes;
    float max_p = 0.0f;
    for (uint8_t c = 0; c < n_classes; ++c) {
        local.p[c] = exps[c] / exp_sum;
        if (local.p[c] > max_p) {
            max_p = local.p[c];
        }
    }
    // D5: confidence = max p — model certainty, never clinical certainty.
    local.confidence = max_p;
    out = local;
    return {Err::Ok};
}

Result<size_t> LinearSoftmaxModel::contributions(const FeatureVector& vector, Contribution* out,
                                                 size_t max) noexcept {
    if (out == nullptr || max == 0) {
        return {Err::InvalidArg, 0};
    }
    // predict() performs the full handshake and value validation, so the
    // probabilities used for argmax are bit-identical to predict()'s output.
    ModelOutput output{};
    const Result<void> predicted = predict(vector, output);
    if (!predicted.ok()) {
        return {predicted.err, 0};
    }

    // D7: contributions of the *predicted* class, in feature order; the bias is
    // excluded (it is not a feature); capped at `max`.
    const uint8_t predicted_class = argmaxOf(output.p, output.n_classes);
    const float* row = data_.weights + static_cast<size_t>(predicted_class) * data_.feature_count;
    const size_t written = vector.count < max ? static_cast<size_t>(vector.count) : max;
    for (size_t i = 0; i < written; ++i) {
        out[i].feature_index = static_cast<uint8_t>(i);
        out[i].weighted_value = row[i] * vector.v[i];
    }
    return {Err::Ok, written};
}

}  // namespace bw::ml
