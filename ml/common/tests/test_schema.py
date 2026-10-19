"""Schema validation tests (F05 spec §9: every listed failure raises with a
field-level message)."""

import copy
import json

import pytest
from schema import K_MAX_FEATURES, SchemaError, load_schema, parse_schema

SCHEMAS_DIR = "ml/common/schemas"
WORKED_EXAMPLE = f"{SCHEMAS_DIR}/feature_schema_v2.json"


def base_schema() -> dict:
    """A fresh valid schema dict (the FEATURE_SCHEMA.md §2.1 worked example)."""
    return {
        "schema_id": 2,
        "schema_version": 1,
        "features": [
            {
                "name": "motion_energy",
                "unit": "g_rms",
                "source": "imu",
                "normalization": {"method": "standardize", "mean": 100.0, "std": 10.0},
            },
            {
                "name": "hr_bpm",
                "unit": "bpm",
                "source": "ppg",
                "normalization": {"method": "none"},
            },
        ],
    }


def expect_error(data: dict, field_fragment: str) -> None:
    """Parse must fail and the message must name the offending field."""
    with pytest.raises(SchemaError) as excinfo:
        parse_schema(data, "test")
    assert field_fragment in str(excinfo.value), str(excinfo.value)


def test_worked_example_loads_with_identity_rewrite() -> None:
    schema = load_schema(WORKED_EXAMPLE)
    assert (schema.schema_id, schema.schema_version, schema.feature_count) == (2, 1, 2)
    motion, hr = schema.features
    assert (motion.method, motion.mean, motion.std) == ("standardize", 100.0, 10.0)
    # method "none" is re-written to identity at load time (D4 / §2.2).
    assert (hr.method, hr.mean, hr.std) == ("none", 0.0, 1.0)


def test_missing_top_level_key() -> None:
    data = base_schema()
    del data["schema_version"]
    expect_error(data, "schema_version")


def test_unknown_top_level_key() -> None:
    data = base_schema()
    data["feature_order"] = "normative?"
    expect_error(data, "feature_order")


def test_duplicate_json_key_rejected() -> None:
    text = json.dumps(base_schema())
    duplicated = text.replace('"schema_id": 2', '"schema_id": 2, "schema_id": 1')
    with pytest.raises(SchemaError) as excinfo:
        parse_schema(json.loads(duplicated, object_pairs_hook=_dup_hook), "test")
    assert "duplicate key" in str(excinfo.value)


def _dup_hook(pairs: list) -> dict:
    seen: dict = {}
    for key, value in pairs:
        if key in seen:
            raise SchemaError(f"{key}: duplicate key (the first occurrence would be silently lost)")
        seen[key] = value
    return seen


def test_schema_id_zero_rejected() -> None:
    data = base_schema()
    data["schema_id"] = 0
    expect_error(data, "schema_id")


def test_schema_version_zero_rejected() -> None:
    data = base_schema()
    data["schema_version"] = 0
    expect_error(data, "schema_version")


def test_boolean_id_rejected() -> None:
    data = base_schema()
    data["schema_id"] = True
    expect_error(data, "schema_id")


def test_feature_count_over_k_max_features_rejected() -> None:
    data = base_schema()
    template = data["features"][0]
    data["features"] = [copy.deepcopy(template) for _ in range(K_MAX_FEATURES + 1)]
    for i in range(len(data["features"])):
        data["features"][i]["name"] = f"feature_{i}"  # keep names unique
    expect_error(data, "features")


def test_empty_features_rejected() -> None:
    data = base_schema()
    data["features"] = []
    expect_error(data, "features")


def test_duplicate_feature_names_rejected() -> None:
    data = base_schema()
    data["features"][1]["name"] = "motion_energy"
    expect_error(data, "duplicate feature name")


def test_unknown_normalization_method_rejected() -> None:
    data = base_schema()
    data["features"][0]["normalization"] = {"method": "minmax", "min": 0.0, "max": 1.0}
    expect_error(data, "normalization.method")


def test_non_positive_std_rejected() -> None:
    data = base_schema()
    data["features"][0]["normalization"] = {"method": "standardize", "mean": 0.0, "std": -1.0}
    expect_error(data, "normalization.std")


def test_standardize_without_std_rejected() -> None:
    data = base_schema()
    data["features"][0]["normalization"] = {"method": "standardize", "mean": 0.0}
    expect_error(data, "normalization")


def test_method_none_with_parameters_rejected() -> None:
    data = base_schema()
    data["features"][1]["normalization"] = {"method": "none", "mean": 0.0, "std": 1.0}
    expect_error(data, "normalization.mean")


def test_non_snake_case_feature_name_rejected() -> None:
    data = base_schema()
    data["features"][0]["name"] = "Motion Energy"
    expect_error(data, "name")


def test_empty_unit_rejected() -> None:
    data = base_schema()
    data["features"][0]["unit"] = ""
    expect_error(data, "unit")


def test_missing_feature_key_rejected() -> None:
    data = base_schema()
    del data["features"][1]["source"]
    expect_error(data, "source")


def test_non_finite_normalization_rejected() -> None:
    # Path 1: a raw NaN literal in the file (load_schema rejects the literal).
    text = json.dumps(base_schema()).replace('"std": 10.0', '"std": NaN')
    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bad.json"
        path.write_text(text, encoding="utf-8")
        with pytest.raises(SchemaError) as excinfo:
            load_schema(path)
        assert "non-finite number literal" in str(excinfo.value)
    # Path 2: a parsed non-finite value reaching the number check (e.g. 1e999).
    data = base_schema()
    data["features"][0]["normalization"]["std"] = float("inf")
    with pytest.raises(SchemaError) as excinfo:
        parse_schema(data, "test")
    assert "finite" in str(excinfo.value)
