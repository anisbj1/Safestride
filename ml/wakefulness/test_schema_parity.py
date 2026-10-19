"""Parity: JSON schema <-> firmware mirror header (FEATURE_SCHEMA.md §3).

F06 §6 D6: alertness_schema.h is a hand-maintained mirror of
ml/wakefulness/schemas/feature_schema_v1.json. These tests fail the pytest
gate whenever the two disagree, so the mirror cannot rot into a fork.
"""

import re
from pathlib import Path

import numpy as np
import pytest
from features import DEFAULT_SCHEMA, SchemaError, load_schema

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "firmware" / "lib" / "bw_wake" / "include" / "bw" / "wake" / "alertness_schema.h"


@pytest.fixture(scope="module")
def schema():
    return load_schema(DEFAULT_SCHEMA)


@pytest.fixture(scope="module")
def header_text():
    assert HEADER.is_file(), f"{HEADER}: firmware mirror header missing"
    return HEADER.read_text(encoding="utf-8")


def _c_array(header_text: str, name: str) -> list[str]:
    m = re.search(rf"{name}\[[^\]]*\]\s*=\s*\{{(.*?)\}};", header_text, re.S)
    assert m, f"{HEADER}: array {name} not found"
    body = re.sub(r"//[^\n]*", "", m.group(1))  # strip comments
    return [t.strip() for t in body.split(",") if t.strip()]


def test_schema_is_alertness_v1(schema):
    assert schema.schema_id == 1  # FEATURE_SCHEMA §2.4 registry row for F06
    assert schema.schema_version == 1
    assert len(schema.features) == 16


def test_header_id_version_and_count_match(schema, header_text):
    got_id = re.search(r"kAlertnessSchemaId\s*=\s*(\d+)", header_text)
    got_ver = re.search(r"kAlertnessSchemaVersion\s*=\s*(\d+)", header_text)
    got_cnt = re.search(r"kAlertnessFeatureCount\s*=\s*(\d+)", header_text)
    assert got_id and got_ver and got_cnt, f"{HEADER}: schema constants missing"
    assert int(got_id.group(1)) == schema.schema_id
    assert int(got_ver.group(1)) == schema.schema_version
    assert int(got_cnt.group(1)) == len(schema.features)


def test_names_in_order_match(schema, header_text):
    names = _c_array(header_text, "kAlertnessFeatureNames")
    assert [n.strip('"') for n in names] == list(schema.names)


def _f(x: str) -> float:
    return float(x.rstrip("fF"))  # C++ float literal: 0.05f


def test_mean_and_std_match(schema, header_text):
    means = [_f(x) for x in _c_array(header_text, "kAlertnessFeatureMean")]
    stds = [_f(x) for x in _c_array(header_text, "kAlertnessFeatureStd")]
    assert len(means) == len(stds) == len(schema.features)
    for i, f in enumerate(schema.features):
        # §2.2: "none" is stored as identity (mean 0, std 1) in the header.
        assert means[i] == pytest.approx(f.normalization.mean), f.name
        assert stds[i] == pytest.approx(f.normalization.std), f.name


def test_normalize_formula_matches_contract(schema):
    # FEATURE_SCHEMA §4 worked example shape: (x - mean) / std.
    raw = {"enmo_mean_g": 0.15, "hr_bpm": 72.0}
    v = schema.normalize(raw)
    assert v[0] == pytest.approx((0.15 - 0.05) / 0.10)
    assert v[1] == pytest.approx(0.0)
    assert len(v) == len(schema.features)


def test_missing_feature_lands_at_identity(schema):
    v = schema.normalize({})
    assert np.all(v == 0.0)  # F06 D9: never invent a value
    v2 = schema.normalize({"enmo_mean_g": float("nan")})
    assert v2[0] == 0.0


def test_duplicate_name_rejected(tmp_path):
    bad = tmp_path / "dup.json"
    bad.write_text(
        '{"schema_id": 1, "schema_version": 1, "features": ['
        '{"name": "a", "unit": "u", "source": "s", "normalization": {"method": "none"}},'
        '{"name": "a", "unit": "u", "source": "s", "normalization": {"method": "none"}}]}',
        encoding="utf-8",
    )
    with pytest.raises(SchemaError, match="duplicate"):
        load_schema(bad)


def test_nonpositive_std_rejected(tmp_path):
    bad = tmp_path / "std.json"
    bad.write_text(
        '{"schema_id": 1, "schema_version": 1, "features": ['
        '{"name": "a", "unit": "u", "source": "s", '
        '"normalization": {"method": "standardize", "mean": 0, "std": 0}}]}',
        encoding="utf-8",
    )
    with pytest.raises(SchemaError, match="std"):
        load_schema(bad)


def test_missing_file_names_path(tmp_path):
    with pytest.raises(SchemaError, match="not found"):
        load_schema(tmp_path / "nope.json")
