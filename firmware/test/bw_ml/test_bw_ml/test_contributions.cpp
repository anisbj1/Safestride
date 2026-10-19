// bw_ml — contributions tests (F05 spec §9, D7): oracle parity of w_pred,i *
// x_i in feature order, the `max` cap, argument validation, bias exclusion,
// and Err::Unsupported for non-linear models.

#include <unity.h>

#include <bw/ml/model_linear.h>

#include "models_under_test.h"

using bw::Contribution;
using bw::Err;
using bw::FeatureVector;
using bw::IModel;
using bw_ml_test::kParityTolerance;
using bw_ml_test::kTestFeatureCount;
using bw_ml_test::kTestSchemaId;
using bw_ml_test::kTestSchemaVersion;

namespace {

FeatureVector validVector() {
    FeatureVector v{};
    v.schema_id = kTestSchemaId;
    v.schema_version = kTestSchemaVersion;
    v.count = kTestFeatureCount;
    v.v[0] = 1.5f;
    v.v[1] = 68.0f;
    v.t_ms = 21;
    return v;
}

}  // namespace

void test_contributions_fixture_values(void) {
    // Values equal the oracle's w_pred,i * x_i (fixture header), in feature order.
    for (size_t i = 0; i < bw_ml_fixture::kPredictCaseCount; ++i) {
        const bw_ml_fixture::PredictCase& c = bw_ml_fixture::kPredictCases[i];
        if (!bw_ml_test::isLinearKey(c.model) || !c.contributions_supported) {
            continue;
        }
        IModel* model = bw_ml_test::modelFor(c.model);
        TEST_ASSERT_NOT_NULL(model);
        Contribution contrib[bw::kMaxFeatures];
        const bw::Result<size_t> r =
            model->contributions(bw_ml_test::makeVector(c, 3), contrib, bw::kMaxFeatures);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(c.n_contributions), static_cast<int>(r.value));
        for (uint8_t f = 0; f < c.n_contributions; ++f) {
            TEST_ASSERT_EQUAL_UINT8(f, contrib[f].feature_index);
            TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, c.contributions[f],
                                     contrib[f].weighted_value);
        }
    }
}

void test_contributions_feature_index_order(void) {
    // Indices are exactly 0..count-1 in ascending order (feature order, D7).
    IModel* model = bw_ml_test::modelFor("linear_3class");
    TEST_ASSERT_NOT_NULL(model);
    Contribution contrib[bw::kMaxFeatures];
    const bw::Result<size_t> r = model->contributions(validVector(), contrib, bw::kMaxFeatures);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kTestFeatureCount), static_cast<int>(r.value));
    for (uint8_t f = 0; f < kTestFeatureCount; ++f) {
        TEST_ASSERT_EQUAL_UINT8(f, contrib[f].feature_index);
    }
}

void test_contributions_cap_at_max(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    Contribution contrib[2];
    contrib[1].feature_index = 77;  // sentinel: must stay untouched past the cap
    contrib[1].weighted_value = 1.25f;
    const bw::Result<size_t> r = model->contributions(validVector(), contrib, 1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(r.value));
    TEST_ASSERT_EQUAL_UINT8(0, contrib[0].feature_index);
    // linear_toy class weights are [1, 0] for the predicted class here, so the
    // first contribution is 1.0 * x0 (oracle: FEATURE_SCHEMA.md §4).
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 1.5f, contrib[0].weighted_value);
    TEST_ASSERT_EQUAL_UINT8(77, contrib[1].feature_index);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, contrib[1].weighted_value);
}

void test_contributions_zero_max_rejected(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    Contribution contrib[1];
    const bw::Result<size_t> r = model->contributions(validVector(), contrib, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), static_cast<int>(r.err));
}

void test_contributions_nullptr_rejected(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    const bw::Result<size_t> r = model->contributions(validVector(), nullptr, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), static_cast<int>(r.err));
}

void test_contributions_tree_unsupported(void) {
    // Non-linear models may answer Err::Unsupported (bw/model.h); D6 pins it.
    const char* const tree_keys[] = {"tree_toy", "tree_deep"};
    for (const char* key : tree_keys) {
        IModel* model = bw_ml_test::modelFor(key);
        TEST_ASSERT_NOT_NULL(model);
        Contribution contrib[bw::kMaxFeatures];
        const bw::Result<size_t> r = model->contributions(validVector(), contrib, bw::kMaxFeatures);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Unsupported), static_cast<int>(r.err));
    }
}

void test_contributions_bias_excluded(void) {
    // Same weights as linear_toy but a non-zero bias: predict() changes (the
    // bias is part of the logits) while contributions() must not (D7: the bias
    // is not a feature, so w * x stays identical).
    constexpr float kWeights[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    constexpr float kBiasBiased[2] = {0.1f, -0.1f};
    constexpr float kBiasPlain[2] = {0.0f, 0.0f};
    const char* const kNames[2] = {"class0", "class1"};

    const bw::ml::ModelDescriptor plain_desc{"plain", 1,      kTestSchemaId, kTestSchemaVersion,
                                             2,       kNames, "test"};
    const bw::ml::ModelDescriptor biased_desc{"biased", 1,      kTestSchemaId, kTestSchemaVersion,
                                              2,        kNames, "test"};
    const bw::ml::LinearModelData plain{kWeights, kBiasPlain, kTestFeatureCount, plain_desc};
    const bw::ml::LinearModelData biased{kWeights, kBiasBiased, kTestFeatureCount, biased_desc};
    bw::ml::LinearSoftmaxModel plain_model{plain};
    bw::ml::LinearSoftmaxModel biased_model{biased};

    const FeatureVector fv = validVector();
    bw::ModelOutput plain_out{};
    bw::ModelOutput biased_out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(plain_model.predict(fv, plain_out).err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(biased_model.predict(fv, biased_out).err));
    // The bias really reaches the probabilities...
    TEST_ASSERT_TRUE(biased_out.p[0] != plain_out.p[0]);

    Contribution from_plain[2];
    Contribution from_biased[2];
    const bw::Result<size_t> r_plain = plain_model.contributions(fv, from_plain, 2);
    const bw::Result<size_t> r_biased = biased_model.contributions(fv, from_biased, 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r_plain.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r_biased.err));
    // ...but never the contributions.
    for (uint8_t f = 0; f < kTestFeatureCount; ++f) {
        TEST_ASSERT_EQUAL_FLOAT(from_plain[f].weighted_value, from_biased[f].weighted_value);
    }
    // And for x = [1.5, 68] the predicted class is 1 (logits [0.1, 1.4]), so
    // the contributions are w[1] * x = [1.5, 0] — had the bias (or argmax)
    // picked class 0, both values would be 0.0 (w[0] = [0, 0]).
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 1.5f, from_biased[0].weighted_value);
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 0.0f, from_biased[1].weighted_value);
}
