#include <bw/ml/model_tree.h>

#include <cmath>

#include <bw/ml/model_validate.h>

namespace bw::ml {
namespace {

// Structural sanity of the generated arrays before any traversal (§3):
// every tree's view must land inside the model-wide arrays. Descriptor
// checks live in validateDescriptor() and are not repeated here.
Result<void> validateTreeData(const TreeModelData& data) noexcept {
    if (data.nodes == nullptr || data.leaves == nullptr || data.trees == nullptr) {
        return {Err::Corrupt};
    }
    if (data.tree_count == 0 || data.tree_count > kMaxTrees) {
        return {Err::Corrupt};  // degenerate metadata (§3)
    }
    if (data.node_count == 0) {
        return {Err::Corrupt};
    }
    for (uint16_t t = 0; t < data.tree_count; ++t) {
        const TreeView& view = data.trees[t];
        if (view.node_count == 0) {
            return {Err::Corrupt};
        }
        if (static_cast<uint32_t>(view.node_offset) + view.node_count > data.node_count) {
            return {Err::Corrupt};
        }
        if (static_cast<uint32_t>(view.leaf_offset) + view.leaf_count > data.leaf_count) {
            return {Err::Corrupt};
        }
    }
    return {Err::Ok};
}

}  // namespace

Result<void> TreeEnsembleModel::predict(const FeatureVector& vector, ModelOutput& out) noexcept {
    Result<void> check = validateInput(vector, data_.descriptor, data_.feature_count);
    if (!check.ok()) {
        return check;
    }
    check = validateTreeData(data_);
    if (!check.ok()) {
        return check;
    }

    const uint8_t n_classes = data_.descriptor.n_classes;
    float acc[kMaxModelClasses] = {};
    for (uint16_t t = 0; t < data_.tree_count; ++t) {
        const TreeView& view = data_.trees[t];
        uint16_t node = 0;  // root
        unsigned steps = 0;
        for (;;) {
            // Termination guard: the exporter caps depth at kMaxTreeDepth, so
            // more than kMaxTreeDepth + 1 visits means a cycle in corrupt data.
            if (++steps > kMaxTreeDepth + 1) {
                return {Err::Corrupt};
            }
            if (node >= view.node_count) {
                return {Err::Corrupt};
            }
            const TreeNode& split = data_.nodes[view.node_offset + node];
            if (split.feature_index >= data_.feature_count) {
                return {Err::Corrupt};  // would index past FeatureVector::v[]
            }
            const uint16_t child =
                (vector.v[split.feature_index] <= split.threshold) ? split.left : split.right;
            if ((child & kLeafFlag) != 0) {
                const uint16_t leaf_index = static_cast<uint16_t>(child & ~kLeafFlag);
                if (leaf_index >= view.leaf_count) {
                    return {Err::Corrupt};
                }
                const TreeLeaf& leaf = data_.leaves[view.leaf_offset + leaf_index];
                for (uint8_t c = 0; c < n_classes; ++c) {
                    acc[c] += leaf.p[c];
                }
                break;
            }
            node = child;
        }
    }

    // Soft voting (D6): average of the leaf probability vectors — a proper
    // distribution straight from the trees, 1:1 with the Python oracle.
    // Assemble locally; out stays untouched on any Err (§3).
    ModelOutput local{};
    local.n_classes = n_classes;
    float max_p = 0.0f;
    for (uint8_t c = 0; c < n_classes; ++c) {
        local.p[c] = acc[c] / static_cast<float>(data_.tree_count);
        // Leaf rows are validated finite at export; a non-finite average means
        // the embedded constants are corrupt (§3).
        if (!std::isfinite(local.p[c])) {
            return {Err::Corrupt};
        }
        if (local.p[c] > max_p) {
            max_p = local.p[c];
        }
    }
    // D5: confidence = max p — model certainty, never clinical certainty.
    local.confidence = max_p;
    out = local;
    return {Err::Ok};
}

Result<size_t> TreeEnsembleModel::contributions(const FeatureVector&, Contribution*,
                                                size_t) noexcept {
    return {Err::Unsupported, 0};  // contract-sanctioned for non-linear models (D6)
}

}  // namespace bw::ml
