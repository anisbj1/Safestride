#pragma once

// bw/ml/model_tree.h — TreeEnsembleModel (F05 D6): flat TreeNode/TreeLeaf
// arrays per tree (root at index 0) with soft voting (average of every tree's
// leaf probability vector). Traversal is bounded by kMaxTreeDepth so corrupt
// data cannot loop; the model is pure math and knows nothing about sensors,
// states or alerts.

#include <bw/model.h>
#include <bw/result.h>

#include <bw/ml/model_descriptor.h>
#include <bw/ml/model_limits.h>

namespace bw::ml {

// Child-reference flag: bit 15 set = leaf index (within the tree), clear =
// node index (within the tree). [DERIVED] Why bit 15: node counts are bounded
// by 2^kMaxTreeDepth = 256 < 0x8000, so the flag never collides with a node
// index and no separate tag field is needed per node.
constexpr uint16_t kLeafFlag = 0x8000;

// Encode a leaf child reference; used by generated headers (keeps emission
// readable instead of a magic 0x8000|n literal).
constexpr uint16_t leafRef(uint16_t leaf_index) noexcept {
    return static_cast<uint16_t>(kLeafFlag | leaf_index);
}

// One internal split node. Field order is normative: it matches the exporter's
// emission (export_cpp.py) and packs to 12 bytes on both host and target.
struct TreeNode {
    uint16_t feature_index;  // split feature; valid only for internal nodes
    float threshold;         // x[feature_index] <= threshold -> left, else right
    uint16_t left;           // child: node index, or leafRef(...) for a leaf
    uint16_t right;
};

// One leaf's probability row: [class], zero-padded to kMaxModelClasses so all
// leaves share one layout (size is part of the kModelBytes accounting, D6/D8).
struct TreeLeaf {
    float p[kMaxModelClasses];
};

// Per-tree offsets into the model-wide flat node/leaf arrays.
struct TreeView {
    uint16_t node_offset;
    uint16_t node_count;
    uint16_t leaf_offset;
    uint16_t leaf_count;
};

// Constant-array payload emitted by export_cpp.py into model_<name>_v<N>.h.
struct TreeModelData {
    const TreeNode* nodes;
    const TreeLeaf* leaves;
    const TreeView* trees;
    uint16_t node_count;  // totals across all trees — bounds checks (§3)
    uint16_t leaf_count;
    uint16_t tree_count;
    uint8_t feature_count;
    ModelDescriptor descriptor;
};

class TreeEnsembleModel final : public IModel {
public:
    // References generated constants: construction is free, storage is the
    // header's (static, flash-backed); the model must outlive this object.
    explicit TreeEnsembleModel(const TreeModelData& data) noexcept : data_(data) {}

    uint16_t schemaId() const noexcept override { return data_.descriptor.schema_id; }

    Result<void> predict(const FeatureVector&, ModelOutput&) noexcept override;
    // Non-linear model: no per-feature contributions exist (D6, sanctioned by
    // bw/model.h) — always Err::Unsupported.
    Result<size_t> contributions(const FeatureVector&, Contribution*, size_t) noexcept override;

    // App-readable metadata (F05 §7: concrete type, not on IModel — §13).
    const ModelDescriptor& descriptor() const noexcept { return data_.descriptor; }
    const TreeModelData& data() const noexcept { return data_; }

private:
    const TreeModelData& data_;
};

}  // namespace bw::ml
