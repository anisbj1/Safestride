// bw_ml — linear-model tests (F05 spec §9, D7): fixture parity against the
// ml/common/reference.py oracle, probability sums, max-shift behaviour on
// large weights, the monotonicity property, determinism and confidence = max p.

#include <cmath>
#include <cstring>

#include <unity.h>

#include "models_under_test.h"

using bw::Err;
using bw::FeatureVector;
using bw::IModel;
using bw::ModelOutput;
using bw_ml_test::kMonotonicSlack;
using bw_ml_test::kParityTolerance;
using bw_ml_test::kProbSumTolerance;

namespace {

FeatureVector vectorFor(float x0, float x1) {
    FeatureVector v{};
    v.schema_id = bw_ml_test::kTestSchemaId;
    v.schema_version = bw_ml_test::kTestSchemaVersion;
    v.count = bw_ml_test::kTestFeatureCount;
    v.v[0] = x0;
    v.v[1] = x1;
    v.t_ms = 7;
    return v;
}

}  // namespace

void test_linear_fixture_parity(void) {
    for (size_t i = 0; i < bw_ml_fixture::kPredictCaseCount; ++i) {
        const bw_ml_fixture::PredictCase& c = bw_ml_fixture::kPredictCases[i];
        if (bw_ml_test::isLinearKey(c.model)) {
            bw_ml_test::assertFixtureCase(c);
        }
    }
}

void test_linear_probability_sums_to_one(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    const float inputs[][2] = {{1.0f, 72.0f}, {0.0f, 0.0f}, {-3.0f, 55.5f}, {2.5f, 90.0f}};
    for (const auto& input : inputs) {
        ModelOutput out{};
        const bw::Result<void> r = model->predict(vectorFor(input[0], input[1]), out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
        float sum = 0.0f;
        for (uint8_t c = 0; c < out.n_classes; ++c) {
            sum += out.p[c];
        }
        TEST_ASSERT_FLOAT_WITHIN(kProbSumTolerance, 1.0f, sum);
    }
}

namespace {

// [EXPERIMENTAL] Weight magnitudes chosen so a *plain* softmax would overflow
// float32 (exp(3.5e30) = Inf -> Inf/Inf = NaN) while the max-shifted logits
// stay finite — the D7 proof case, values well inside float32 range.
constexpr float kHugeWeights[4] = {1e30f, -1e30f, -2e30f, 1.5e30f};
constexpr float kHugeBias[2] = {0.0f, -1e29f};
const char* const kHugeClassNames[2] = {"class0", "class1"};

}  // namespace

void test_linear_large_weights_finite_and_normalized(void) {
    const bw::ml::ModelDescriptor descriptor{
        "huge_weights",  1,     bw_ml_test::kTestSchemaId, bw_ml_test::kTestSchemaVersion, 2,
        kHugeClassNames, "test"};
    const bw::ml::LinearModelData data{kHugeWeights, kHugeBias, bw_ml_test::kTestFeatureCount,
                                       descriptor};
    bw::ml::LinearSoftmaxModel model{data};

    const float inputs[][2] = {{1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}};
    for (const auto& input : inputs) {
        ModelOutput out{};
        const bw::Result<void> r = model.predict(vectorFor(input[0], input[1]), out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
        float sum = 0.0f;
        for (uint8_t c = 0; c < out.n_classes; ++c) {
            TEST_ASSERT_TRUE(std::isfinite(out.p[c]));
            TEST_ASSERT_TRUE(out.p[c] >= 0.0f && out.p[c] <= 1.0f);
            sum += out.p[c];
        }
        TEST_ASSERT_TRUE(std::isfinite(out.confidence));
        TEST_ASSERT_FLOAT_WITHIN(kProbSumTolerance, 1.0f, sum);
    }
}

void test_linear_monotonic_positive_weight(void) {
    // Property (§9): increasing a positively weighted feature never lowers
    // that class's probability. linear_toy class 1 has w = [1, 0] on feature 0.
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    ModelOutput previous{};
    bw::Result<void> first = model->predict(vectorFor(-5.0f, 60.0f), previous);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(first.err));
    for (int step = 1; step <= 20; ++step) {
        const float x0 = -5.0f + 0.5f * static_cast<float>(step);
        ModelOutput out{};
        const bw::Result<void> r = model->predict(vectorFor(x0, 60.0f), out);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
        TEST_ASSERT_TRUE_MESSAGE(out.p[1] >= previous.p[1] - kMonotonicSlack,
                                 "p(class1) decreased on +x0");
        previous = out;
    }
}

void test_linear_determinism_bit_identical(void) {
    IModel* model = bw_ml_test::modelFor("linear_3class");
    TEST_ASSERT_NOT_NULL(model);
    ModelOutput a{};
    ModelOutput b{};
    const FeatureVector fv = vectorFor(1.5f, 68.0f);
    const bw::Result<void> ra = model->predict(fv, a);
    const bw::Result<void> rb = model->predict(fv, b);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(ra.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(rb.err));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(&a, &b, sizeof(ModelOutput)));
}

void test_linear_timestamp_irrelevant(void) {
    // t_ms must not influence the output (§9): the model is a pure function
    // of the feature values.
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector early = vectorFor(1.0f, 72.0f);
    FeatureVector late = vectorFor(1.0f, 72.0f);
    early.t_ms = 0;
    late.t_ms = 999999;
    ModelOutput a{};
    ModelOutput b{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(model->predict(early, a).err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(model->predict(late, b).err));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(&a, &b, sizeof(ModelOutput)));
}

void test_linear_confidence_is_max_p(void) {
    for (size_t i = 0; i < bw_ml_fixture::kPredictCaseCount; ++i) {
        const bw_ml_fixture::PredictCase& c = bw_ml_fixture::kPredictCases[i];
        if (!bw_ml_test::isLinearKey(c.model)) {
            continue;
        }
        IModel* model = bw_ml_test::modelFor(c.model);
        TEST_ASSERT_NOT_NULL(model);
        ModelOutput out{};
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(Err::Ok),
            static_cast<int>(model->predict(bw_ml_test::makeVector(c, 5), out).err));
        float max_p = 0.0f;
        for (uint8_t k = 0; k < out.n_classes; ++k) {
            if (out.p[k] > max_p) {
                max_p = out.p[k];
            }
        }
        TEST_ASSERT_EQUAL_FLOAT(max_p, out.confidence);
    }
}
