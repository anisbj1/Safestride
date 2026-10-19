"""Feature-schema loading and validation (F05 D2, docs/contracts/FEATURE_SCHEMA.md §2.1).

``ml/common/schemas/feature_schema_v<N>.json`` (N = schema id) is the single
source of truth for feature order, units and normalization constants: the same
file drives Python training code and the generated C++ constants. A defect here
must fail loudly on the Python side and never reach firmware (FEATURE_SCHEMA.md
§3 error table).

Every failure raises :class:`SchemaError` with a field-level message such as
``features[1].normalization.std: must be > 0, got -1.0``.
"""

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Mirrors bw::kMaxFeatures (firmware/lib/bw_core/include/bw/model.h); a test in
# ml/common/tests/test_export.py parses that header and fails if the two drift.
K_MAX_FEATURES = 32

# uint16 bounds for schema_id / schema_version (bw contract types, 02 §1).
K_UINT16_MAX = 65535

# snake_case pattern — FEATURE_SCHEMA.md §2.1 requires non-empty snake_case
# names for features (and F05 uses it for model names / class names too).
_SNAKE_CASE = re.compile(r"[a-z][a-z0-9_]*")

_TOP_LEVEL_KEYS = ("schema_id", "schema_version", "features")
_FEATURE_KEYS = ("name", "unit", "source", "normalization")
_NORMALIZATION_METHODS = ("none", "standardize")
# "method: none" carries no parameters (they would be silently ignored);
# "standardize" requires exactly mean/std. FEATURE_SCHEMA.md §2.1.
_NONE_KEYS = ("method",)
_STANDARDIZE_KEYS = ("method", "mean", "std")


class SchemaError(ValueError):
    """Any schema defect; the message always names the offending field."""


def _error(field: str, message: str) -> SchemaError:
    return SchemaError(f"{field}: {message}")


def _require_dict(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise _error(field, f"expected an object, got {type(value).__name__}")
    return value


def _reject_constant(name: str) -> None:
    # json.loads accepts NaN/Infinity literals by default; a non-finite
    # normalization constant must never be emitted into firmware (§2.1).
    raise _error("<json>", f"non-finite number literal {name!r} is not allowed")


def _load_no_duplicate_keys(text: str) -> Any:
    def hook(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        seen: dict[str, Any] = {}
        for key, value in pairs:
            if key in seen:
                raise _error(key, "duplicate key (the first occurrence would be silently lost)")
            seen[key] = value
        return seen

    return json.loads(text, object_pairs_hook=hook, parse_constant=_reject_constant)


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


def _require_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _error(field, f"expected a number, got {type(value).__name__}")
    number = float(value)
    if number != number or number in (float("inf"), float("-inf")):
        raise _error(field, f"must be finite, got {value}")
    return number


def _require_non_empty_string(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise _error(field, f"expected a string, got {type(value).__name__}")
    if not value:
        raise _error(field, "must not be empty")
    return value


@dataclass(frozen=True, slots=True)
class Feature:
    """One schema feature (FEATURE_SCHEMA.md §2.1)."""

    name: str
    unit: str
    source: str
    method: str  # "none" | "standardize" (never used at runtime: "none" is
    # re-written to identity constants at generation time, D4)
    mean: float  # 0.0 for method "none" (identity)
    std: float  # 1.0 for method "none" (identity)


@dataclass(frozen=True, slots=True)
class FeatureSchema:
    """A loaded, fully validated feature schema."""

    schema_id: int
    schema_version: int
    features: tuple[Feature, ...]

    @property
    def feature_count(self) -> int:
        return len(self.features)


def _parse_normalization(raw: Any, field: str) -> tuple[str, float, float]:
    """Return (method, mean, std) constants for the normative formula x' = (x - mean) / std."""
    norm = _require_dict(raw, field)
    method = _require_non_empty_string(norm.get("method"), f"{field}.method")
    if method not in _NORMALIZATION_METHODS:
        raise _error(
            f"{field}.method",
            f"unknown method {method!r}, expected one of {list(_NORMALIZATION_METHODS)}",
        )
    if method == "none":
        _check_keys(norm, _NONE_KEYS, field)
        return method, 0.0, 1.0  # identity — FEATURE_SCHEMA.md §2.2
    _check_keys(norm, _STANDARDIZE_KEYS, field)
    mean = _require_number(norm["mean"], f"{field}.mean")
    std = _require_number(norm["std"], f"{field}.std")
    if std <= 0.0:
        raise _error(f"{field}.std", f"must be > 0, got {std}")
    return method, mean, std


def _parse_feature(raw: Any, index: int, seen_names: set[str]) -> Feature:
    field = f"features[{index}]"
    feat = _require_dict(raw, field)
    _check_keys(feat, _FEATURE_KEYS, field)
    name = _require_non_empty_string(feat["name"], f"{field}.name")
    if not _SNAKE_CASE.fullmatch(name):
        raise _error(f"{field}.name", f"{name!r} is not snake_case")
    if name in seen_names:
        raise _error(f"{field}.name", f"duplicate feature name {name!r}")
    seen_names.add(name)
    unit = _require_non_empty_string(feat["unit"], f"{field}.unit")
    source = _require_non_empty_string(feat["source"], f"{field}.source")
    method, mean, std = _parse_normalization(feat["normalization"], f"{field}.normalization")
    return Feature(name=name, unit=unit, source=source, method=method, mean=mean, std=std)


def parse_schema(data: Any, origin: str) -> FeatureSchema:
    """Validate an already-parsed JSON document; ``origin`` labels error messages."""
    root = _require_dict(data, origin)
    _check_keys(root, _TOP_LEVEL_KEYS, origin)
    schema_id = _require_int(root["schema_id"], f"{origin}.schema_id", 1, K_UINT16_MAX)
    schema_version = _require_int(
        root["schema_version"], f"{origin}.schema_version", 1, K_UINT16_MAX
    )
    raw_features = root["features"]
    if not isinstance(raw_features, list):
        raise _error(f"{origin}.features", f"expected an array, got {type(raw_features).__name__}")
    if not 1 <= len(raw_features) <= K_MAX_FEATURES:
        raise _error(
            f"{origin}.features",
            f"must contain 1..{K_MAX_FEATURES} features, got {len(raw_features)}",
        )
    seen: set[str] = set()
    features = tuple(_parse_feature(item, i, seen) for i, item in enumerate(raw_features))
    return FeatureSchema(schema_id=schema_id, schema_version=schema_version, features=features)


def load_schema(path: str | Path) -> FeatureSchema:
    """Load and validate one schema file; :class:`SchemaError` names the file and field."""
    file_path = Path(path)
    try:
        text = file_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SchemaError(f"{file_path}: cannot read schema file ({exc.strerror})") from exc
    try:
        data = _load_no_duplicate_keys(text)
    except SchemaError as exc:
        raise SchemaError(f"{file_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise SchemaError(
            f"{file_path}: invalid JSON at line {exc.lineno} column {exc.colno}: {exc.msg}"
        ) from exc
    return parse_schema(data, str(file_path))
