// bw_ml — handshake tests (F05 spec §9, D3): schema_id / schema_version /
// count are validated BEFORE v[] is read, NaN/Inf features are refused,
// degenerate embedded metadata is Err::Corrupt, and on any Err the output
// argument is left untouched (FEATURE_SCHEMA.md §3).

#include <limits>

#include <unity.h>

#include <bw/ml/model_linear.h>
#include <bw/ml/model_tree.h>

#include "models_under_test.h"

using bw::Contribution;
using bw::Err;
using bw::FeatureVector;
using bw::IModel;
using bw::ModelOutput;
using bw::Result;
using bw_ml_test::kTestFeatureCount;
using bw_ml_test::kTestSchemaId;
using bw_ml_test::kTestSchemaVersion;

namespace {

// A vector the fixture models accept (FEATURE_SCHEMA.md §4 input).
FeatureVector validVector() {
    FeatureVector v{};
    v.schema_id = kTestSchemaId;
    v.schema_version = kTestSchemaVersion;
    v.count = kTestFeatureCount;
    v.v[0] = 1.0f;
    v.v[1] = 72.0f;
    v.t_ms = 42;
    return v;
}

int errOf(const Result<void>& result) {
    return static_cast<int>(result.err);
}

// ModelOutput comparators (field-wise: padding bytes are not part of the
// contract, and the suite compares what callers can actually read).
void fillOutputSentinel(ModelOutput& out) {
    out.n_classes = 7;
    out.confidence = 0.25f;
    for (uint8_t i = 0; i < bw::kMaxModelClasses; ++i) {
        out.p[i] = 0.5f + 0.01f * static_cast<float>(i);
    }
}

void assertSameOutput(const ModelOutput& expected, const ModelOutput& actual) {
    TEST_ASSERT_EQUAL_UINT8(expected.n_classes, actual.n_classes);
    TEST_ASSERT_EQUAL_FLOAT(expected.confidence, actual.confidence);
    for (uint8_t i = 0; i < bw::kMaxModelClasses; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(expected.p[i], actual.p[i]);
    }
}

// Degenerate-metadata payload builders (a generated model never contains
// these; §3 requires the defensive Err).
constexpr float kWeights[4] = {0.0f, 0.0f, 1.0f, 0.0f};
constexpr float kBias[2] = {0.0f, 0.0f};
const char* const kClassNames[2] = {"class0", "class1"};

}  // namespace

void test_handshake_wrong_schema_id_rejected(void) {
    IModel* linear = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(linear);
    FeatureVector fv = validVector();
    fv.schema_id = static_cast<uint16_t>(kTestSchemaId + 1);
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(linear->predict(fv, out)));
}

void test_handshake_tree_wrong_schema_version_rejected(void) {
    // The same rules hold for the other family (§3 is family-independent).
    IModel* tree = bw_ml_test::modelFor("tree_toy");
    TEST_ASSERT_NOT_NULL(tree);
    FeatureVector fv = validVector();
    fv.schema_version = static_cast<uint16_t>(kTestSchemaVersion + 1);
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(tree->predict(fv, out)));
}

void test_handshake_tree_wrong_schema_id_rejected(void) {
    // The tree family must reject a foreign feature set by id, exactly like
    // the linear one (§3 is family-independent).
    IModel* tree = bw_ml_test::modelFor("tree_toy");
    TEST_ASSERT_NOT_NULL(tree);
    FeatureVector fv = validVector();
    fv.schema_id = static_cast<uint16_t>(kTestSchemaId + 1);
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(tree->predict(fv, out)));
}

void test_handshake_wrong_schema_version_rejected(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector fv = validVector();
    fv.schema_version = static_cast<uint16_t>(kTestSchemaVersion + 1);
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(model->predict(fv, out)));
}

void test_handshake_count_mismatch_rejected(void) {
    // 0, +1 and 255 must all fail before v[] is read — a count of 255 would
    // read far past FeatureVector::v[kMaxFeatures] if it were not (D3).
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    const uint8_t counts[] = {0, static_cast<uint8_t>(kTestFeatureCount + 1), 255};
    for (uint8_t count : counts) {
        FeatureVector fv = validVector();
        fv.count = count;
        ModelOutput out{};
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(model->predict(fv, out)));
    }
}

void test_handshake_nan_feature_rejected(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector fv = validVector();
    fv.v[1] = std::numeric_limits<float>::quiet_NaN();
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(model->predict(fv, out)));
}

void test_handshake_inf_feature_rejected(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector fv = validVector();
    fv.v[0] = std::numeric_limits<float>::infinity();
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(model->predict(fv, out)));
}

void test_handshake_zero_classes_corrupt(void) {
    const bw::ml::ModelDescriptor descriptor{
        "degenerate", 1, kTestSchemaId, kTestSchemaVersion, 0, kClassNames, "test"};
    const bw::ml::LinearModelData data{kWeights, kBias, kTestFeatureCount, descriptor};
    bw::ml::LinearSoftmaxModel model{data};
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
}

void test_handshake_null_class_names_corrupt(void) {
    const bw::ml::ModelDescriptor descriptor{
        "degenerate", 1, kTestSchemaId, kTestSchemaVersion, 2, nullptr, "test"};
    const bw::ml::LinearModelData data{kWeights, kBias, kTestFeatureCount, descriptor};
    bw::ml::LinearSoftmaxModel model{data};
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
}

void test_handshake_zero_trees_corrupt(void) {
    constexpr bw::ml::TreeNode kNodes[1] = {
        bw::ml::TreeNode{0, 0.5f, bw::ml::leafRef(0), bw::ml::leafRef(1)},
    };
    constexpr bw::ml::TreeLeaf kLeaves[2] = {
        bw::ml::TreeLeaf{{0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        bw::ml::TreeLeaf{{0.2f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    };
    constexpr bw::ml::TreeView kViews[1] = {bw::ml::TreeView{0, 1, 0, 2}};
    const bw::ml::ModelDescriptor descriptor{
        "degenerate", 1, kTestSchemaId, kTestSchemaVersion, 2, kClassNames, "test"};
    const bw::ml::TreeModelData data{kNodes, kLeaves,           kViews,    1, 2,
                                     0,      kTestFeatureCount, descriptor};
    bw::ml::TreeEnsembleModel model{data};
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
}

void test_handshake_predict_output_untouched_on_error(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector fv = validVector();
    fv.schema_id = static_cast<uint16_t>(kTestSchemaId + 1);
    ModelOutput out{};
    fillOutputSentinel(out);
    const ModelOutput sentinel = out;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), errOf(model->predict(fv, out)));
    assertSameOutput(sentinel, out);
}

void test_handshake_contributions_output_untouched_on_error(void) {
    IModel* model = bw_ml_test::modelFor("linear_toy");
    TEST_ASSERT_NOT_NULL(model);
    FeatureVector fv = validVector();
    fv.count = 255;
    Contribution contrib[bw::kMaxFeatures];
    for (uint8_t i = 0; i < bw::kMaxFeatures; ++i) {
        contrib[i].feature_index = 77;
        contrib[i].weighted_value = 1.25f;
    }
    const bw::Result<size_t> result = model->contributions(fv, contrib, bw::kMaxFeatures);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::InvalidArg), static_cast<int>(result.err));
    for (uint8_t i = 0; i < bw::kMaxFeatures; ++i) {
        TEST_ASSERT_EQUAL_UINT8(77, contrib[i].feature_index);
        TEST_ASSERT_EQUAL_FLOAT(1.25f, contrib[i].weighted_value);
    }
}

// --- defensive payloads: no generated header can contain these (§3) ---------

void test_handshake_tree_cycle_corrupt(void) {
    // Both children of the single node point back at node 0, so the walk would
    // never reach a leaf. The exporter caps depth at kMaxTreeDepth, so the
    // traversal must give up at the kMaxTreeDepth + 1 visit guard rather than
    // spin forever (model_tree.cpp termination guard).
    constexpr bw::ml::TreeNode kCycleNodes[1] = {
        bw::ml::TreeNode{0, 0.5f, 0, 0},
    };
    constexpr bw::ml::TreeLeaf kCycleLeaves[2] = {
        bw::ml::TreeLeaf{{0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        bw::ml::TreeLeaf{{0.2f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    };
    constexpr bw::ml::TreeView kCycleViews[1] = {bw::ml::TreeView{0, 1, 0, 2}};
    const bw::ml::ModelDescriptor descriptor{
        "cycle", 1, kTestSchemaId, kTestSchemaVersion, 2, kClassNames, "test"};
    const bw::ml::TreeModelData data{kCycleNodes, kCycleLeaves,      kCycleViews, 1, 2,
                                     1,           kTestFeatureCount, descriptor};
    bw::ml::TreeEnsembleModel model{data};
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
}

void test_handshake_tree_out_of_range_index_corrupt(void) {
    constexpr bw::ml::TreeLeaf kLeaves[2] = {
        bw::ml::TreeLeaf{{0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        bw::ml::TreeLeaf{{0.2f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    };
    constexpr bw::ml::TreeView kViews[1] = {bw::ml::TreeView{0, 1, 0, 2}};
    const bw::ml::ModelDescriptor descriptor{
        "out_of_range", 1, kTestSchemaId, kTestSchemaVersion, 2, kClassNames, "test"};
    ModelOutput out{};
    fillOutputSentinel(out);
    const ModelOutput sentinel = out;

    // (a) Non-leaf child index past the view's node array (node_count = 1).
    constexpr bw::ml::TreeNode kBadChild[1] = {
        bw::ml::TreeNode{0, 0.5f, 3, 3},
    };
    const bw::ml::TreeModelData child_data{kBadChild, kLeaves,           kViews,    1, 2,
                                           1,         kTestFeatureCount, descriptor};
    bw::ml::TreeEnsembleModel child_model{child_data};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt),
                          errOf(child_model.predict(validVector(), out)));
    assertSameOutput(sentinel, out);

    // (b) Leaf index past the view's leaf array (leaf_count = 2).
    constexpr bw::ml::TreeNode kBadLeaf[1] = {
        bw::ml::TreeNode{0, 0.5f, bw::ml::leafRef(5), bw::ml::leafRef(5)},
    };
    const bw::ml::TreeModelData leaf_data{kBadLeaf, kLeaves,           kViews,    1, 2,
                                          1,        kTestFeatureCount, descriptor};
    bw::ml::TreeEnsembleModel leaf_model{leaf_data};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt),
                          errOf(leaf_model.predict(validVector(), out)));
    assertSameOutput(sentinel, out);
}

void test_handshake_tree_out_of_range_feature_index_corrupt(void) {
    // Split on feature 2 while the schema/vector only defines 0..1: reading
    // v[2] would index past FeatureVector::v[] (§3 bounds rule).
    constexpr bw::ml::TreeNode kNodes[1] = {
        bw::ml::TreeNode{2, 0.5f, bw::ml::leafRef(0), bw::ml::leafRef(1)},
    };
    constexpr bw::ml::TreeLeaf kLeaves[2] = {
        bw::ml::TreeLeaf{{0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        bw::ml::TreeLeaf{{0.2f, 0.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    };
    constexpr bw::ml::TreeView kViews[1] = {bw::ml::TreeView{0, 1, 0, 2}};
    const bw::ml::ModelDescriptor descriptor{
        "bad_feature", 1, kTestSchemaId, kTestSchemaVersion, 2, kClassNames, "test"};
    const bw::ml::TreeModelData data{kNodes, kLeaves,           kViews,    1, 2,
                                     1,      kTestFeatureCount, descriptor};
    bw::ml::TreeEnsembleModel model{data};
    ModelOutput out{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
}

void test_handshake_linear_weights_overflow_corrupt(void) {
    // Hand-built payload (a generated header never contains weights this
    // large): with the fixture vector v = [1.0, 72.0] the dot product is
    // 3e38 + 72 * 3e38, which overflows to +inf. Max-shift cannot repair that,
    // so predict() must refuse (Err::Corrupt) and leave the caller's output
    // untouched (§3).
    constexpr float kHugeWeights[4] = {3.0e38f, 3.0e38f, 3.0e38f, 3.0e38f};
    const bw::ml::ModelDescriptor descriptor{
        "overflow", 1, kTestSchemaId, kTestSchemaVersion, 2, kClassNames, "test"};
    const bw::ml::LinearModelData data{kHugeWeights, kBias, kTestFeatureCount, descriptor};
    bw::ml::LinearSoftmaxModel model{data};
    ModelOutput out{};
    fillOutputSentinel(out);
    const ModelOutput sentinel = out;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Corrupt), errOf(model.predict(validVector(), out)));
    assertSameOutput(sentinel, out);
}
