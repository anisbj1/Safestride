"""Feature-schema loading and normalization for F06 wakefulness.

Contract: docs/contracts/FEATURE_SCHEMA.md §2.1 (JSON layout and validation)
and §4 (normalization formula). The JSON schema is the source of truth;
firmware consumes the mirrored header bw/wake/alertness_schema.h, kept in
parity by test_schema_parity.py.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np

METHODS = ("none", "standardize")
MAX_FEATURES = 32  # bw/model.h kMaxFeatures (FEATURE_SCHEMA §2.1)


class SchemaError(ValueError):
    """Schema is invalid; the message names the file path and the field."""


@dataclass(frozen=True)
class Normalization:
    method: str
    mean: float = 0.0  # identity for "none" (FEATURE_SCHEMA §2.2)
    std: float = 1.0


@dataclass(frozen=True)
class FeatureSpec:
    name: str
    unit: str
    source: str
    normalization: Normalization


@dataclass(frozen=True)
class FeatureSchema:
    path: Path
    schema_id: int
    schema_version: int
    features: tuple[FeatureSpec, ...]

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(f.name for f in self.features)

    def feature(self, name: str) -> FeatureSpec:
        for f in self.features:
            if f.name == name:
                return f
        raise SchemaError(f"{self.path}: unknown feature {name!r}")

    def normalize(self, values: dict[str, float]) -> np.ndarray:
        """Normalize name->value into contract order (§4: (x-mean)/std).

        Missing entries land at the normalization identity — normalized 0,
        i.e. the schema mean for "standardize", 0 for "none" (F06 §6 D9):
        no value is invented, the companion missingness feature says so.
        """
        row = []
        for f in self.features:
            x = values.get(f.name)
            if x is None or (isinstance(x, float) and math.isnan(x)):
                row.append(0.0)
                continue
            n = (float(x) - f.normalization.mean) / f.normalization.std
            row.append(n)
        return np.asarray(row, dtype=np.float64)


def load_schema(path: str | Path) -> FeatureSchema:
    p = Path(path)
    if not p.is_file():
        raise SchemaError(f"{p}: schema file not found")
    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SchemaError(f"{p}: unparsable JSON ({exc})") from exc

    schema_id = _uint16(p, raw, "schema_id")
    schema_version = _uint16(p, raw, "schema_version")
    feats_raw = raw.get("features")
    if not isinstance(feats_raw, list) or not feats_raw:
        raise SchemaError(f"{p}: features must be a non-empty array")
    if len(feats_raw) > MAX_FEATURES:
        raise SchemaError(
            f"{p}: features has {len(feats_raw)} entries > kMaxFeatures={MAX_FEATURES}"
        )

    features: list[FeatureSpec] = []
    seen: set[str] = set()
    for i, f in enumerate(feats_raw):
        where = f"{p}: features[{i}]"
        if not isinstance(f, dict):
            raise SchemaError(f"{where}: expected an object")
        name = _str(where, f, "name")
        if not name.isascii() or not all(c.isalnum() or c == "_" for c in name):
            raise SchemaError(f"{where}: name {name!r} is not snake_case")
        if name in seen:
            raise SchemaError(f"{where}: duplicate feature name {name!r}")
        seen.add(name)
        unit = _str(where, f, "unit")
        source = _str(where, f, "source")
        norm_raw = f.get("normalization")
        if not isinstance(norm_raw, dict):
            raise SchemaError(f"{where}: missing object 'normalization'")
        method = norm_raw.get("method")
        if method not in METHODS:
            raise SchemaError(f"{where}: normalization.method {method!r} not in {METHODS}")
        mean, std = 0.0, 1.0  # §2.2: "none" is rewritten to identity
        if method == "standardize":
            mean = norm_raw.get("mean")
            std = norm_raw.get("std")
            if not isinstance(mean, (int, float)) or not math.isfinite(mean):
                raise SchemaError(f"{where}: standardize.mean must be a finite float")
            if not isinstance(std, (int, float)) or not math.isfinite(std) or std <= 0:
                raise SchemaError(f"{where}: standardize.std must be a finite float > 0")
            mean, std = float(mean), float(std)
        features.append(
            FeatureSpec(
                name=name,
                unit=unit,
                source=source,
                normalization=Normalization(method=method, mean=mean, std=std),
            )
        )

    return FeatureSchema(
        path=p,
        schema_id=schema_id,
        schema_version=schema_version,
        features=tuple(features),
    )


def _uint16(p: Path, raw: dict, key: str) -> int:
    v = raw.get(key)
    if not isinstance(v, int) or isinstance(v, bool) or not (0 < v <= 0xFFFF):
        raise SchemaError(f"{p}: {key} must be a uint16 != 0 (got {v!r})")
    return v


def _str(where: str, f: dict, key: str) -> str:
    v = f.get(key)
    if not isinstance(v, str) or not v:
        raise SchemaError(f"{where}: {key} must be a non-empty string")
    return v


DEFAULT_SCHEMA = Path(__file__).parent / "schemas" / "feature_schema_v1.json"
