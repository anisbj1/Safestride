"""Exporter / oracle tests (F05 spec §9): determinism, staleness (D8),
float fidelity, mirror checks, and the committed worked-example expectations."""

import json
import re
import struct
from pathlib import Path

import export_cpp
import pytest
from export_cpp import (
    REPO_ROOT,
    check_all,
    float32_lit,
    generate_model_header,
    iter_artifacts,
    model_byte_size,
)
from reference import (
    K_LEAF_FLAG,
    K_MAX_MODEL_BYTES,
    K_MAX_TREE_DEPTH,
    K_MAX_TREES,
    FlatNode,
    FlatTree,
    ModelError,
    TreeModel,
    confidence_of,
    contributions,
    load_model,
    predict,
    softmax,
)
from schema import K_MAX_FEATURES

MODELS_DIR = REPO_ROOT / "ml" / "common" / "models"

# FEATURE_SCHEMA.md §4 prints the worked-example values to 8 decimals, so the
# hand-value comparisons tolerate 1e-8 (the oracle itself is exact float64).
K_HAND_VALUE_TOLERANCE = 1e-8


def _define_int(header: Path, name: str) -> int:
    pattern = rf"constexpr\s+\w+\s+{name}\s*=\s*(\d+)\s*;"
    match = re.search(pattern, header.read_text(encoding="utf-8"))
    assert match, f"{name} not found in {header}"
    return int(match.group(1))


# --------------------------------------------------------------------------
# D8: determinism, staleness, float fidelity, budgets
# --------------------------------------------------------------------------


def test_export_deterministic_two_runs_identical() -> None:
    assert iter_artifacts() == iter_artifacts()


def test_generated_headers_not_stale() -> None:
    """The D8 staleness gate: every checked-in header equals fresh oracle output."""
    problems = check_all()
    assert problems == [], "run: python ml/common/export_cpp.py all\n" + "\n".join(problems)


@pytest.mark.parametrize(
    "value",
    [0.1, 1.0 / 3.0, 1e-8, -2.5, 42.0, 1e20, 3.0e38, -0.0, 1.1754944e-38],
)
def test_float32_lit_round_trips_bit_exact(value: float) -> None:
    literal = float32_lit(value)
    assert literal.endswith("f")
    parsed = float(literal[:-1])
    assert struct.pack("f", parsed) == struct.pack("f", value)


def test_every_generated_artifact_is_format_guarded() -> None:
    """D8: generated layout must never be re-wrapped by the repo format gate.

    The staleness test alone cannot catch this — a full revert (generator +
    regenerated files) would still be byte-identical, so assert the guards
    themselves bracket every artifact's content.
    """
    artifacts = iter_artifacts()
    assert artifacts, "no artifacts to guard"
    for path, payload in artifacts:
        text = payload.decode("utf-8")
        lines = [line.strip() for line in text.splitlines()]
        off = lines.index(export_cpp.FORMAT_GUARD_OFF)
        on = lines.index(export_cpp.FORMAT_GUARD_ON)
        # off comes from the banner (before the content), on closes the file
        assert off < on, f"{path.name}: guard order wrong"
        assert on == len(lines) - 1 or not any(lines[on + 1 :]), (
            f"{path.name}: content after clang-format on"
        )
        # everything that could be re-wrapped sits inside the guarded span
        assert lines[off - 1].startswith("// "), f"{path.name}: off not in banner"


def test_model_limits_header_mirrors_python_constants() -> None:
    header = export_cpp.MODEL_LIMITS_HEADER
    assert _define_int(header, "kMaxTreeDepth") == K_MAX_TREE_DEPTH
    assert _define_int(header, "kMaxTrees") == K_MAX_TREES
    assert _define_int(header, "kMaxModelBytes") == K_MAX_MODEL_BYTES


def test_bw_core_header_mirrors_python_constants() -> None:
    header = REPO_ROOT / "firmware" / "lib" / "bw_core" / "include" / "bw" / "model.h"
    assert _define_int(header, "kMaxFeatures") == K_MAX_FEATURES
    assert _define_int(header, "kMaxModelClasses") == 8


def test_committed_models_within_byte_budget() -> None:
    for path in sorted(MODELS_DIR.glob("model_*.json")):
        model = load_model(path)
        assert 0 < model_byte_size(model) <= K_MAX_MODEL_BYTES


def _full_tree(depth: int) -> dict:
    """A complete subtree: `depth` split levels, leaves = one-hot class vectors."""
    if depth == 0:
        return {"leaf": [1.0, 0.0]}
    child = _full_tree(depth - 1)
    return {"feature": 0, "threshold": 0.0, "left": child, "right": child}


def test_over_budget_model_rejected_at_export() -> None:
    # Four complete depth-8 trees: 4 * (511 nodes * 12 + 256 leaves * 32 + 8) > 20480.
    tree = FlatTree(
        nodes=tuple(
            FlatNode(feature=0, threshold=0.0, left=K_LEAF_FLAG, right=K_LEAF_FLAG)
            for _ in range(511)
        ),
        leaves=tuple((1.0, 0.0) for _ in range(256)),
    )
    model = TreeModel(
        name="over_budget",
        family="tree",
        model_version=1,
        schema_id=2,
        schema_version=1,
        feature_count=1,
        classes=("class0", "class1"),
        training_data_hash="test:over-budget",
        trees=(tree,) * 4,
    )
    assert model_byte_size(model) > K_MAX_MODEL_BYTES
    with pytest.raises(ValueError, match="kMaxModelBytes"):
        generate_model_header(model, "synthetic.json")


# --------------------------------------------------------------------------
# Oracle properties and the committed worked examples (FEATURE_SCHEMA.md §4)
# --------------------------------------------------------------------------


def test_softmax_large_logits_stay_finite_and_normalized() -> None:
    probabilities = softmax([1e5, -1e5, 3.0])
    assert all(0.0 <= value <= 1.0 for value in probabilities)
    assert sum(probabilities) == pytest.approx(1.0, abs=1e-12)
    assert probabilities[0] == pytest.approx(1.0, abs=1e-12)


def test_linear_worked_example_oracle() -> None:
    model = load_model(MODELS_DIR / "model_linear_toy_v1.json")
    probabilities = predict(model, [1.0, 72.0])
    assert probabilities == pytest.approx([0.26894142, 0.73105858], abs=K_HAND_VALUE_TOLERANCE)
    assert sum(probabilities) == pytest.approx(1.0, abs=1e-12)
    assert confidence_of(probabilities) == pytest.approx(0.73105858, abs=K_HAND_VALUE_TOLERANCE)
    weighted = [value for _, value in contributions(model, [1.0, 72.0])]
    assert weighted == pytest.approx([1.0, 0.0], abs=1e-12)


def test_tree_worked_example_oracle() -> None:
    model = load_model(MODELS_DIR / "model_tree_toy_v1.json")
    # x0 > 0.5: both trees vote right (soft average of the two right leaves).
    assert predict(model, [1.0, 72.0]) == pytest.approx([0.3, 0.7], abs=1e-12)
    # x0 <= 0.5 goes left, boundary included (the firmware tie rule).
    assert predict(model, [0.0, 0.0]) == pytest.approx([0.7, 0.3], abs=1e-12)
    assert predict(model, [0.5, 0.0]) == pytest.approx([0.7, 0.3], abs=1e-12)
    with pytest.raises(ModelError):
        contributions(model, [1.0, 72.0])


def test_oracle_fixture_cases_are_normalized() -> None:
    models = {name: load_model(path) for name, path in _model_paths().items()}
    cases = export_cpp.load_cases()
    assert len(cases) >= 10
    for case in cases:
        model = models[case["model"]]
        x = [float(value) for value in case["v"]]
        probabilities = predict(model, x)
        assert len(probabilities) == model.n_classes
        assert sum(probabilities) == pytest.approx(1.0, abs=1e-9)
        assert confidence_of(probabilities) == max(probabilities)


def _model_paths() -> dict[str, Path]:
    paths = {}
    for path in sorted(MODELS_DIR.glob("model_*.json")):
        model = load_model(path)
        paths[model.name] = path
    return paths


# --------------------------------------------------------------------------
# Model-file validation (export-time guards, FEATURE_SCHEMA.md §3)
# --------------------------------------------------------------------------


def _load_tmp(tmp_path: Path, payload: dict) -> None:
    path = tmp_path / "model_x_v1.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    load_model(path)  # raises ModelError on invalid payloads


def _tree_payload() -> dict:
    return {
        "name": "x",
        "family": "tree",
        "model_version": 1,
        "schema_id": 2,
        "schema_version": 1,
        "feature_count": 1,
        "classes": ["class0", "class1"],
        "training_data_hash": "test:tree",
        "trees": [
            {
                "feature": 0,
                "threshold": 0.5,
                "left": {"leaf": [1.0, 0.0]},
                "right": {"leaf": [0.0, 1.0]},
            }
        ],
    }


def test_leaf_probabilities_must_sum_to_one(tmp_path: Path) -> None:
    payload = _tree_payload()
    payload["trees"][0]["left"] = {"leaf": [0.9, 0.05]}
    with pytest.raises(ModelError, match="sum to 1"):
        _load_tmp(tmp_path, payload)


def test_tree_depth_limit_enforced_at_load(tmp_path: Path) -> None:
    payload = _tree_payload()
    payload["trees"][0]["left"] = _full_tree(K_MAX_TREE_DEPTH)  # leaf at depth 8: allowed
    payload["trees"][0]["right"] = _full_tree(K_MAX_TREE_DEPTH + 1)  # one level too deep
    with pytest.raises(ModelError, match="kMaxTreeDepth"):
        _load_tmp(tmp_path, payload)


def test_tree_count_limit_enforced_at_load(tmp_path: Path) -> None:
    payload = _tree_payload()
    payload["trees"] = [payload["trees"][0]] * (K_MAX_TREES + 1)
    with pytest.raises(ModelError, match="kMaxTrees"):
        _load_tmp(tmp_path, payload)


def test_linear_row_length_must_match_schema(tmp_path: Path) -> None:
    payload = {
        "name": "x",
        "family": "linear",
        "model_version": 1,
        "schema_id": 2,
        "schema_version": 1,
        "feature_count": 2,
        "classes": ["class0", "class1"],
        "training_data_hash": "test:linear",
        "weights": [[0.0, 1.0], [1.0]],
        "bias": [0.0, 0.0],
    }
    with pytest.raises(ModelError, match="weights\\[1\\]"):
        _load_tmp(tmp_path, payload)
