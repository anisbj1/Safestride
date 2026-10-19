// firmware/test/bw_ml/test_bw_ml/models_under_test.h — shared helpers for the
// F05 host suite: the fixture-model factory, FeatureVector construction and
// one end-to-end fixture-case assertion (F05 §9 parity checks). Header-only so
// no extra translation unit has to be registered in test_main.cpp.

#pragma once

#include <cstring>

#include <unity.h>

#include <bw/ml/model_linear.h>
#include <bw/ml/model_linear_3class_v1.h>
#include <bw/ml/model_linear_toy_v1.h>
#include <bw/ml/model_tree.h>
#include <bw/ml/model_tree_deep_v1.h>
#include <bw/ml/model_tree_toy_v1.h>

#include "fixture_cases.h"

namespace bw_ml_test {

// [EXPERIMENTAL] Max |firmware (float32) - oracle (float64)| accepted by the §9
// parity checks; FEATURE_SCHEMA.md §4 names it kParityTolerance.
constexpr float kParityTolerance = 1e-5f;
// [EXPERIMENTAL] Accepted |sum(p) - 1| (§9), same name as in §4.
constexpr float kProbSumTolerance = 1e-5f;
// [EXPERIMENTAL] Slack for the monotonicity property (§9): strictly smaller
// than kParityTolerance so a real violation cannot hide inside float noise.
constexpr float kMonotonicSlack = 1e-6f;

// Handshake values of the fixture schema/model (FEATURE_SCHEMA.md §4).
// schema_id 2: id 1 is reserved for F06's alertness set (FEATURE_SCHEMA.md §2.4).
constexpr uint16_t kTestSchemaId = 2;
constexpr uint16_t kTestSchemaVersion = 1;
constexpr uint8_t kTestFeatureCount = 2;

// Returns nullptr for an unknown key so a stale fixture case fails with an
// assertion instead of undefined behaviour.
inline bw::IModel* modelFor(const char* name) {
    static bw::ml::LinearSoftmaxModel linear_toy{bw::ml::linear_toy::kModel};
    static bw::ml::LinearSoftmaxModel linear_3class{bw::ml::linear_3class::kModel};
    static bw::ml::TreeEnsembleModel tree_toy{bw::ml::tree_toy::kModel};
    static bw::ml::TreeEnsembleModel tree_deep{bw::ml::tree_deep::kModel};
    if (std::strcmp(name, "linear_toy") == 0) {
        return &linear_toy;
    }
    if (std::strcmp(name, "linear_3class") == 0) {
        return &linear_3class;
    }
    if (std::strcmp(name, "tree_toy") == 0) {
        return &tree_toy;
    }
    if (std::strcmp(name, "tree_deep") == 0) {
        return &tree_deep;
    }
    return nullptr;
}

// Fixture-case keys are emitted from the model file names (export_cpp.py).
inline bool isLinearKey(const char* name) {
    return std::strncmp(name, "linear_", 7) == 0;
}

// FeatureVector from a fixture case; t_ms is caller-set so tests can prove the
// timestamp has no influence on the output (§9).
inline bw::FeatureVector makeVector(const bw_ml_fixture::PredictCase& c, bw::TimeMs t_ms) {
    bw::FeatureVector v{};
    v.schema_id = c.schema_id;
    v.schema_version = c.schema_version;
    v.count = c.count;
    v.t_ms = t_ms;
    for (uint8_t i = 0; i < bw::kMaxFeatures; ++i) {
        v.v[i] = c.v[i];
    }
    return v;
}

// Asserts one fixture case end-to-end: predict parity, probability sum,
// confidence = max p, and contributions (values, order, or Err::Unsupported).
inline void assertFixtureCase(const bw_ml_fixture::PredictCase& c) {
    bw::IModel* model = modelFor(c.model);
    TEST_ASSERT_NOT_NULL_MESSAGE(model, c.model);
    const bw::FeatureVector fv = makeVector(c, 1234);
    bw::ModelOutput out{};
    const bw::Result<void> result = model->predict(fv, out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Ok), static_cast<int>(result.err));
    TEST_ASSERT_EQUAL_UINT8(c.n_classes, out.n_classes);
    float sum = 0.0f;
    for (uint8_t i = 0; i < c.n_classes; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, c.p[i], out.p[i]);
        sum += out.p[i];
    }
    TEST_ASSERT_FLOAT_WITHIN(kProbSumTolerance, 1.0f, sum);
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, c.confidence, out.confidence);

    bw::Contribution contrib[bw::kMaxFeatures];
    const bw::Result<size_t> r = model->contributions(fv, contrib, bw::kMaxFeatures);
    if (c.contributions_supported) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Ok), static_cast<int>(r.err));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(c.n_contributions), static_cast<int>(r.value));
        for (uint8_t i = 0; i < c.n_contributions; ++i) {
            TEST_ASSERT_EQUAL_UINT8(i, contrib[i].feature_index);
            TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, c.contributions[i],
                                     contrib[i].weighted_value);
        }
    } else {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Unsupported), static_cast<int>(r.err));
    }
}

}  // namespace bw_ml_test
