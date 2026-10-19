// bw_ml — tree-ensemble tests (F05 spec §9, D6): fixture path parity, soft
// voting, the <= threshold boundary rule, traversal depth bounded by
// kMaxTreeDepth, and the tree/model size limits of §8.

#include <unity.h>

#include <bw/ml/model_limits.h>
#include <bw/ml/model_tree.h>

#include "models_under_test.h"

using bw::Err;
using bw::FeatureVector;
using bw::IModel;
using bw::ModelOutput;
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
    v.t_ms = 11;
    return v;
}

// Depth (in edges) of the deepest path from the root of one tree; also proves
// every child reference stays inside this tree's node array.
uint16_t treeDepth(const bw::ml::TreeModelData& data, uint16_t tree_index) {
    const bw::ml::TreeView& view = data.trees[tree_index];
    // Explicit stack: at most kMaxTreeDepth + 1 entries (export enforces the
    // depth), so the walk needs no recursion and no dynamic storage.
    uint16_t nodes[bw::ml::kMaxTreeDepth + 1] = {0};   // node index
    uint16_t depths[bw::ml::kMaxTreeDepth + 1] = {0};  // depth at that node
    uint16_t max_depth = 0;
    int top = 0;
    nodes[0] = 0;
    depths[0] = 0;
    while (top >= 0) {
        const uint16_t index = nodes[top];
        const uint16_t depth = depths[top];
        --top;
        const bw::ml::TreeNode& node = data.nodes[view.node_offset + index];
        TEST_ASSERT_TRUE_MESSAGE(depth <= bw::ml::kMaxTreeDepth, "tree deeper than kMaxTreeDepth");
        TEST_ASSERT_TRUE_MESSAGE(node.feature_index < data.feature_count,
                                 "split feature outside the schema");
        const uint16_t children[2] = {node.left, node.right};
        for (uint16_t child : children) {
            if ((child & bw::ml::kLeafFlag) != 0) {
                const uint16_t leaf = child & ~bw::ml::kLeafFlag;
                TEST_ASSERT_TRUE_MESSAGE(leaf < view.leaf_count, "leaf index out of range");
                // The leaf edge itself adds one level beyond this split node.
                if (static_cast<uint16_t>(depth + 1) > max_depth) {
                    max_depth = static_cast<uint16_t>(depth + 1);
                }
            } else {
                TEST_ASSERT_TRUE_MESSAGE(child < view.node_count, "node index out of range");
                TEST_ASSERT_TRUE_MESSAGE(top + 1 < bw::ml::kMaxTreeDepth + 1, "stack overflow");
                ++top;
                nodes[top] = child;
                depths[top] = static_cast<uint16_t>(depth + 1);
            }
        }
    }
    return max_depth;
}

}  // namespace

void test_tree_fixture_parity(void) {
    for (size_t i = 0; i < bw_ml_fixture::kPredictCaseCount; ++i) {
        const bw_ml_fixture::PredictCase& c = bw_ml_fixture::kPredictCases[i];
        if (!bw_ml_test::isLinearKey(c.model)) {
            bw_ml_test::assertFixtureCase(c);
        }
    }
}

void test_tree_soft_vote_and_boundary_rule(void) {
    IModel* model = bw_ml_test::modelFor("tree_toy");
    TEST_ASSERT_NOT_NULL(model);
    // x0 > 0.5: both trees vote right -> average of [0.2,0.8] and [0.4,0.6].
    ModelOutput right{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(model->predict(vectorFor(1.0f, 72.0f), right).err));
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 0.3f, right.p[0]);
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 0.7f, right.p[1]);
    TEST_ASSERT_FLOAT_WITHIN(kProbSumTolerance, 1.0f, right.p[0] + right.p[1]);
    // x0 <= 0.5 goes left (boundary included — the firmware tie rule, §4).
    ModelOutput left{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(model->predict(vectorFor(0.5f, 0.0f), left).err));
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 0.7f, left.p[0]);
    TEST_ASSERT_FLOAT_WITHIN(kParityTolerance, 0.3f, left.p[1]);
    // Here class0 wins with 0.7 — confidence is max p whatever the vote is.
    TEST_ASSERT_EQUAL_FLOAT(left.p[0], left.confidence);
}

void test_tree_probability_sums_to_one(void) {
    for (size_t i = 0; i < bw_ml_fixture::kPredictCaseCount; ++i) {
        const bw_ml_fixture::PredictCase& c = bw_ml_fixture::kPredictCases[i];
        if (bw_ml_test::isLinearKey(c.model)) {
            continue;
        }
        IModel* model = bw_ml_test::modelFor(c.model);
        TEST_ASSERT_NOT_NULL(model);
        ModelOutput out{};
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(Err::Ok),
            static_cast<int>(model->predict(bw_ml_test::makeVector(c, 9), out).err));
        float sum = 0.0f;
        for (uint8_t k = 0; k < out.n_classes; ++k) {
            sum += out.p[k];
        }
        TEST_ASSERT_FLOAT_WITHIN(kProbSumTolerance, 1.0f, sum);
    }
}

void test_tree_depth_within_limit(void) {
    const bw::ml::TreeModelData& toy = bw::ml::tree_toy::kModel;
    const bw::ml::TreeModelData& deep = bw::ml::tree_deep::kModel;
    for (uint16_t t = 0; t < toy.tree_count; ++t) {
        const uint16_t depth = treeDepth(toy, t);
        TEST_ASSERT_TRUE(depth >= 1);
        TEST_ASSERT_TRUE(depth <= bw::ml::kMaxTreeDepth);
    }
    uint16_t deepest = 0;
    for (uint16_t t = 0; t < deep.tree_count; ++t) {
        const uint16_t depth = treeDepth(deep, t);
        TEST_ASSERT_TRUE(depth <= bw::ml::kMaxTreeDepth);
        if (depth > deepest) {
            deepest = depth;
        }
    }
    // The fixture must actually exercise multi-level traversal (§9).
    TEST_ASSERT_TRUE_MESSAGE(deepest >= 2, "tree_deep fixture is not deeper than 1");
}

void test_tree_counts_within_budget(void) {
    // §8 limits, read from the generated headers.
    TEST_ASSERT_TRUE(bw::ml::tree_toy::kModel.tree_count <= bw::ml::kMaxTrees);
    TEST_ASSERT_TRUE(bw::ml::tree_deep::kModel.tree_count <= bw::ml::kMaxTrees);
    TEST_ASSERT_TRUE(bw::ml::tree_toy::kModelBytes <= bw::ml::kMaxModelBytes);
    TEST_ASSERT_TRUE(bw::ml::tree_deep::kModelBytes <= bw::ml::kMaxModelBytes);
    TEST_ASSERT_TRUE(bw::ml::linear_toy::kModelBytes <= bw::ml::kMaxModelBytes);
    TEST_ASSERT_TRUE(bw::ml::linear_3class::kModelBytes <= bw::ml::kMaxModelBytes);
}
