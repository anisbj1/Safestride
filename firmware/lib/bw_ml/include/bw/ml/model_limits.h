#pragma once

// bw/ml/model_limits.h — F05 spec §8: the three model-size limits.
// Python mirrors these in ml/common/reference.py (K_MAX_TREE_DEPTH /
// K_MAX_TREES / K_MAX_MODEL_BYTES); ml/common/tests/test_export.py parses this
// header and fails if the two ever drift, and the generated model headers
// static_assert kModelBytes <= kMaxModelBytes.

#include <cstddef>

namespace bw::ml {

// [EXPERIMENTAL] Bounds flash and latency of exported trees: traversal runs at
// most kMaxTreeDepth + 1 node visits (termination guard in TreeEnsembleModel),
// and export_cpp.py refuses deeper trees.
constexpr unsigned kMaxTreeDepth = 8;

// [EXPERIMENTAL] Bounds model size; revisit after F29 results (F05 §8).
constexpr unsigned kMaxTrees = 32;

// [EXPERIMENTAL] Flash budget (F05 §11) as an enforceable limit: checked by
// export_cpp.py at generation and static_asserted on the generated model's
// kModelBytes. Why not implied by the depth/count limits: a single full
// depth-8 tree alone is ~511 nodes (~14 KB), so depth/count bounds do not
// imply the byte budget (F05 D6).
constexpr size_t kMaxModelBytes = 20480;

}  // namespace bw::ml
