"""Pure-Python prediction oracle and model loader (F05 §7).

This is the reference implementation the firmware parity tests are written
against ("firmware matches Python" must be testable, not aspirational — F05
spec §7) and the source the exporter reads its constant arrays from.

Precision: the oracle computes in float64 while firmware computes in float32;
the difference stays orders of magnitude below the committed parity tolerance
(``kParityTolerance = 1e-5``, FEATURE_SCHEMA.md §4), so a single tolerance
covers both.

Model files (``ml/common/models/<name>_v<N>.json``, N = model_version) are an
F05-internal artifact: training code writes them, ``export_cpp.py`` and this
module read them. Layout is documented in the exported header comments.
"""

import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from schema import K_MAX_FEATURES

# Mirrors bw::kMaxModelClasses (bw/model.h) — bounds ModelOutput::p and leaf vectors.
K_MAX_CLASSES = 8
# Mirrors F05 spec §8 (firmware/lib/bw_ml/include/bw/ml/model_limits.h);
# test_export.py parses that header and fails if the two drift.
K_MAX_TREE_DEPTH = 8
K_MAX_TREES = 32
K_MAX_MODEL_BYTES = 20480
# Child-reference bit in TreeNode::left/right: set = leaf index, clear = node index
# (mirrors bw::ml::kLeafFlag in model_tree.h).
K_LEAF_FLAG = 0x8000
# Per-tree leaf probability sum must equal 1 within this tolerance; it is checked
# at load time so a malformed model can never be exported or voted on.
# [EXPERIMENTAL] 1e-6 is far below the 1e-5 firmware parity tolerance while
# staying loose enough for decimal-authored JSON (e.g. 0.1 + 0.2 + 0.7).
K_LEAF_SUM_TOLERANCE = 1e-6

_SNAKE_CASE = re.compile(r"[a-z][a-z0-9_]*")
# training_data_hash travels into a C++ string literal: restrict the charset so
# no escaping rules are needed (a hash is hex/base32 text anyway).
_HASH_CHARSET = re.compile(r"[A-Za-z0-9:_.\-]+")

_COMMON_KEYS = (
    "name",
    "family",
    "model_version",
    "schema_id",
    "schema_version",
    "feature_count",
    "classes",
    "training_data_hash",
)
_LINEAR_KEYS = _COMMON_KEYS + ("weights", "bias")
_TREE_KEYS = _COMMON_KEYS + ("trees",)
_NODE_KEYS = ("feature", "threshold", "left", "right")
_LEAF_KEYS = ("leaf",)


class ModelError(ValueError):
    """Any model defect; the message always names the offending field."""


def _error(field: str, message: str) -> ModelError:
    return ModelError(f"{field}: {message}")


def _require_dict(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise _error(field, f"expected an object, got {type(value).__name__}")
    return value


def _check_keys(obj: dict[str, Any], expected: tuple[str, ...], field: str) -> None:
    for key in expected:
        if key not in obj:
            raise _error(field, f"missing key {key!r}")
    for key in obj:
        if key not in expected:
            raise _error(f"{field}.{key}", "unknown key")


def _require_int(value: Any, field: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _error(field, f"expected an integer, got {type(value).__name__}")
    if not low <= value <= high:
        raise _error(field, f"must be within {low}..{high}, got {value}")
    return value


def _require_float(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _error(field, f"expected a number, got {type(value).__name__}")
    number = float(value)
    if not math.isfinite(number):
        raise _error(field, f"must be finite, got {value}")
    return number


@dataclass(frozen=True, slots=True)
class FlatNode:
    """One internal tree node in flat (already-flattened) form.

    ``left``/``right`` are child references: node index, or ``K_LEAF_FLAG |
    leaf_index`` for a leaf — the exact encoding firmware traverses (D6).
    """

    feature: int
    threshold: float
    left: int
    right: int


@dataclass(frozen=True, slots=True)
class FlatTree:
    """One tree: flat node array (root at index 0) + flat leaf probability rows."""

    nodes: tuple[FlatNode, ...]
    leaves: tuple[tuple[float, ...], ...]


@dataclass(frozen=True, slots=True)
class ModelBase:
    """Fields shared by both model families (embedded metadata, D2)."""

    name: str
    family: str
    model_version: int
    schema_id: int
    schema_version: int
    feature_count: int
    classes: tuple[str, ...]
    training_data_hash: str

    @property
    def n_classes(self) -> int:
        return len(self.classes)


@dataclass(frozen=True, slots=True)
class LinearModel(ModelBase):
    """Row-major ``weights[class][feature]`` plus per-class ``bias`` (D7)."""

    weights: tuple[tuple[float, ...], ...]
    bias: tuple[float, ...]


@dataclass(frozen=True, slots=True)
class TreeModel(ModelBase):
    """Soft-voting ensemble of flat trees (D6)."""

    trees: tuple[FlatTree, ...]


Model = LinearModel | TreeModel


def _parse_common(
    raw: dict[str, Any], expected_keys: tuple[str, ...], field: str
) -> dict[str, Any]:
    _check_keys(raw, expected_keys, field)
    name = raw["name"]
    if not isinstance(name, str) or not _SNAKE_CASE.fullmatch(name):
        raise _error(f"{field}.name", f"{name!r} is not snake_case")
    family = raw["family"]
    if family not in ("linear", "tree"):
        raise _error(f"{field}.family", f"unknown family {family!r}, expected 'linear' or 'tree'")
    training_hash = raw["training_data_hash"]
    if (
        not isinstance(training_hash, str)
        or not training_hash
        or not _HASH_CHARSET.fullmatch(training_hash)
    ):
        raise _error(
            f"{field}.training_data_hash",
            "must be a non-empty string of [A-Za-z0-9:_.-] (it becomes a C++ literal)",
        )
    classes_raw = raw["classes"]
    if not isinstance(classes_raw, list) or not 1 <= len(classes_raw) <= K_MAX_CLASSES:
        raise _error(
            f"{field}.classes",
            f"must be an array of 1..{K_MAX_CLASSES} class names, got {classes_raw!r}",
        )
    classes: list[str] = []
    for i, cls in enumerate(classes_raw):
        if not isinstance(cls, str) or not _SNAKE_CASE.fullmatch(cls):
            raise _error(f"{field}.classes[{i}]", f"{cls!r} is not snake_case")
        if cls in classes:
            raise _error(f"{field}.classes[{i}]", f"duplicate class name {cls!r}")
        classes.append(cls)
    return {
        "name": name,
        "family": family,
        "model_version": _require_int(raw["model_version"], f"{field}.model_version", 1, 2**32 - 1),
        "schema_id": _require_int(raw["schema_id"], f"{field}.schema_id", 1, 65535),
        "schema_version": _require_int(raw["schema_version"], f"{field}.schema_version", 1, 65535),
        "feature_count": _require_int(
            raw["feature_count"], f"{field}.feature_count", 1, K_MAX_FEATURES
        ),
        "classes": tuple(classes),
        "training_data_hash": training_hash,
    }


def _parse_linear(raw: dict[str, Any], field: str) -> LinearModel:
    common = _parse_common(raw, _LINEAR_KEYS, field)
    n_classes = len(common["classes"])
    feature_count = common["feature_count"]
    weights_raw = raw["weights"]
    if not isinstance(weights_raw, list) or len(weights_raw) != n_classes:
        raise _error(
            f"{field}.weights",
            f"expected {n_classes} rows (one per class), got {weights_raw!r}",
        )
    weights: list[tuple[float, ...]] = []
    for c, row in enumerate(weights_raw):
        if not isinstance(row, list) or len(row) != feature_count:
            raise _error(
                f"{field}.weights[{c}]",
                f"expected {feature_count} floats (one per feature), got {row!r}",
            )
        weights.append(
            tuple(_require_float(v, f"{field}.weights[{c}][{i}]") for i, v in enumerate(row))
        )
    bias_raw = raw["bias"]
    if not isinstance(bias_raw, list) or len(bias_raw) != n_classes:
        raise _error(f"{field}.bias", f"expected {n_classes} floats, got {bias_raw!r}")
    bias = tuple(_require_float(v, f"{field}.bias[{i}]") for i, v in enumerate(bias_raw))
    return LinearModel(**common, weights=tuple(weights), bias=bias)


def _is_leaf(node: dict[str, Any]) -> bool:
    return set(node) == set(_LEAF_KEYS)


def _flatten_tree(root: dict[str, Any], field: str, n_classes: int, feature_count: int) -> FlatTree:
    """Flatten nested JSON into the exact arrays firmware traverses (D6, pre-order)."""
    nodes: list[FlatNode] = []
    leaves: list[tuple[float, ...]] = []

    def visit(sub: Any, path: str, depth: int) -> int:
        if depth > K_MAX_TREE_DEPTH:
            raise _error(path, f"tree deeper than kMaxTreeDepth = {K_MAX_TREE_DEPTH}")
        node = _require_dict(sub, path)
        if _is_leaf(node):
            row_raw = node["leaf"]
            if not isinstance(row_raw, list) or len(row_raw) != n_classes:
                raise _error(
                    f"{path}.leaf",
                    f"expected {n_classes} probabilities (one per class), got {row_raw!r}",
                )
            row = tuple(_require_float(v, f"{path}.leaf[{i}]") for i, v in enumerate(row_raw))
            for i, probability in enumerate(row):
                if not 0.0 <= probability <= 1.0:
                    raise _error(f"{path}.leaf[{i}]", f"must be within 0..1, got {probability}")
            if abs(sum(row) - 1.0) > K_LEAF_SUM_TOLERANCE:
                raise _error(f"{path}.leaf", f"probabilities must sum to 1, got {sum(row)}")
            leaves.append(row)
            return K_LEAF_FLAG | (len(leaves) - 1)
        _check_keys(node, _NODE_KEYS, path)
        feature = _require_int(node["feature"], f"{path}.feature", 0, feature_count - 1)
        threshold = _require_float(node["threshold"], f"{path}.threshold")
        index = len(nodes)
        nodes.append(None)  # placeholder: pre-order index is claimed before children
        left = visit(node["left"], f"{path}.left", depth + 1)
        right = visit(node["right"], f"{path}.right", depth + 1)
        nodes[index] = FlatNode(feature=feature, threshold=threshold, left=left, right=right)
        return index

    root_index = visit(root, field, depth=0)
    if root_index & K_LEAF_FLAG:
        raise _error(field, "tree root must be a split node, not a leaf")
    return FlatTree(nodes=tuple(nodes), leaves=tuple(leaves))


def _parse_tree(raw: dict[str, Any], field: str) -> TreeModel:
    common = _parse_common(raw, _TREE_KEYS, field)
    feature_count = common["feature_count"]
    trees_raw = raw["trees"]
    if not isinstance(trees_raw, list) or not 1 <= len(trees_raw) <= K_MAX_TREES:
        raise _error(
            f"{field}.trees",
            f"expected 1..{K_MAX_TREES} trees (kMaxTrees), got {trees_raw!r}",
        )
    trees = tuple(
        _flatten_tree(tree, f"{field}.trees[{i}]", len(common["classes"]), feature_count)
        for i, tree in enumerate(trees_raw)
    )
    return TreeModel(**common, trees=trees)


def load_model(path: str | Path) -> Model:
    """Load and validate one model file; :class:`ModelError` names file and field."""
    file_path = Path(path)
    try:
        text = file_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ModelError(f"{file_path}: cannot read model file ({exc.strerror})") from exc
    try:
        raw = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ModelError(
            f"{file_path}: invalid JSON at line {exc.lineno} column {exc.colno}: {exc.msg}"
        ) from exc
    root = _require_dict(raw, str(file_path))
    family = root.get("family")
    if family == "linear":
        parsed: Model = _parse_linear(root, str(file_path))
    elif family == "tree":
        parsed = _parse_tree(root, str(file_path))
    else:
        raise _error(
            f"{file_path}.family",
            f"unknown family {family!r}, expected 'linear' or 'tree'",
        )
    return parsed


def softmax(logits: list[float]) -> list[float]:
    """Max-shifted softmax (D7): subtract max(logits) before exp so large but
    finite logits can never overflow to Inf/NaN."""
    top = max(logits)
    exps = [math.exp(z - top) for z in logits]
    total = sum(exps)
    return [value / total for value in exps]


def argmax(probabilities: list[float]) -> int:
    """First index of the maximum — the tie rule firmware uses (deterministic)."""
    best = 0
    for i in range(1, len(probabilities)):
        if probabilities[i] > probabilities[best]:
            best = i
    return best


def confidence_of(probabilities: list[float]) -> float:
    """ModelOutput.confidence = max p (D5): model certainty, never clinical certainty."""
    return max(probabilities)


def _check_input(model: Model, x: list[float]) -> None:
    if len(x) != model.feature_count:
        raise ModelError(
            f"model {model.name}: expected {model.feature_count} features, got {len(x)}"
        )
    for i, value in enumerate(x):
        if not math.isfinite(value):
            raise ModelError(f"x[{i}]: must be finite, got {value}")


def predict(model: Model, x: list[float]) -> list[float]:
    """Return the per-class probability vector (oracle used by tests and the exporter)."""
    _check_input(model, x)
    if isinstance(model, LinearModel):
        logits = [
            model.bias[c] + sum(w * v for w, v in zip(model.weights[c], x, strict=True))
            for c in range(model.n_classes)
        ]
        return softmax(logits)
    acc = [0.0] * model.n_classes
    for t, tree in enumerate(model.trees):
        index = 0
        steps = 0
        while True:
            steps += 1
            if steps > K_MAX_TREE_DEPTH + 1:
                raise ModelError(
                    f"model {model.name}: tree {t} did not terminate within kMaxTreeDepth"
                )
            node = tree.nodes[index]
            child = node.left if x[node.feature] <= node.threshold else node.right
            if child & K_LEAF_FLAG:
                leaf = tree.leaves[child & ~K_LEAF_FLAG]
                for c in range(model.n_classes):
                    acc[c] += leaf[c]
                break
            index = child
    return [total / len(model.trees) for total in acc]


def predicted_class(probabilities: list[float]) -> int:
    return argmax(probabilities)


def contributions(model: Model, x: list[float]) -> list[tuple[int, float]]:
    """(feature_index, w_pred,i * x_i) for the predicted class, feature order (D7).

    Raises :class:`ModelError` for non-linear models — firmware answers
    ``Err::Unsupported`` there (contract-sanctioned by bw/model.h).
    """
    if not isinstance(model, LinearModel):
        raise ModelError(f"model {model.name}: contributions() is only defined for linear models")
    probabilities = predict(model, x)
    row = model.weights[argmax(probabilities)]
    return [(i, row[i] * x[i]) for i in range(model.feature_count)]
