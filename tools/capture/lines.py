"""F28 D6: research-row classification and parsing (DATASET_FORMAT.md §2).

`is_research_line` decides whether a serial line belongs to the dataset;
everything else is diagnostics and must never enter a CSV. `parse_line`
validates a research line against schema v1 and returns ``(kind, row)``.
Malformed input is COUNTED by the caller and discarded — never repaired,
never guessed (F28 §10).
"""

from __future__ import annotations

# kSessionSchemaVersion in firmware/lib/bw_research/.../research_config.h.
SCHEMA_VERSION = 1

QUALITY_NAMES = frozenset({"Good", "Degraded", "Insufficient"})
ACTIVITY_NAMES = frozenset(
    {
        "Unknown",
        "Stationary",
        "Sitting",
        "Standing",
        "Walking",
        "Running",
        "LyingResting",
        "InactiveProlonged",
    }
)
EVENT_TYPE_NAMES = frozenset(
    {
        "SleepEpisode",
        "DrowsinessEvent",
        "SuspectedCataplexy",
        "UserMarked",
        "AlertRaised",
        "SensorFault",
        "DeviceNotWorn",
        "BatteryWarning",
        "NapPlanned",
        "NapCompleted",
        "TimeSync",
        "Boot",
    }
)
USER_LABEL_NAMES = frozenset({"Unlabeled", "Confirmed", "Rejected"})
ALERTNESS_NAMES = frozenset(
    {"Unknown", "NotWorn", "Awake", "Drowsy", "HighDrowsinessRisk", "PossibleSleep"}
)


class MalformedRow(ValueError):
    """A research-prefixed line that fails the schema v1 grammar."""


class SchemaMismatch(RuntimeError):
    """A #HDR announced a schema version this tool does not implement."""


def is_research_line(line: str) -> bool:
    """DATASET_FORMAT.md §2.4: the exact classification rule."""
    line = line.rstrip("\r\n")
    if line.startswith("#HDR,"):
        return True
    if len(line) < 3 or line[0] not in "IPFEM" or line[1] != ",":
        return False
    second = line.split(",", 2)[1]
    if line[0] == "F":
        return second in {"M", "P", "C"}
    return second[:1].isdigit() or (second[:1] == "-" and second[1:2].isdigit())


def _split(line: str, expect: int) -> list[str]:
    fields = line.rstrip("\r\n").split(",")
    if len(fields) != expect:
        raise MalformedRow(f"expected {expect} fields, got {len(fields)}: {line[:60]!r}")
    return fields


def _uint(text: str, name: str) -> int:
    try:
        value = int(text)
    except ValueError as exc:
        raise MalformedRow(f"{name} is not an integer: {text!r}") from exc
    if value < 0:
        raise MalformedRow(f"{name} must be unsigned: {text!r}")
    return value


def _float(text: str, name: str) -> float:
    try:
        return float(text)
    except ValueError as exc:
        raise MalformedRow(f"{name} is not a number: {text!r}") from exc


def _bool01(text: str, name: str) -> bool:
    if text == "1":
        return True
    if text == "0":
        return False
    raise MalformedRow(f"{name} must be 0 or 1: {text!r}")


def _enum(text: str, allowed: frozenset[str], name: str) -> str:
    if text not in allowed:
        raise MalformedRow(f"unknown {name} {text!r} (schema drift?)")
    return text


def _parse_header(line: str) -> dict[str, object]:
    fields = _split(line, 8)
    schema = _uint(fields[1], "schema")
    if schema != SCHEMA_VERSION:
        raise SchemaMismatch(f"dataset schema {schema}, tool implements {SCHEMA_VERSION}")
    return {
        "schema_version": schema,
        "boot_id": _uint(fields[2], "boot_id"),
        "firmware_version": fields[3],
        "imu_drops": _uint(fields[4], "imu_drops"),
        "ppg_drops": _uint(fields[5], "ppg_drops"),
        "line_drops": _uint(fields[6], "line_drops"),
        "feature_missed": _uint(fields[7], "feature_missed"),
    }


def _parse_imu(line: str) -> dict[str, object]:
    fields = _split(line, 8)
    return {
        "t_ms": _uint(fields[1], "t_ms"),
        "ax_g": _float(fields[2], "ax_g"),
        "ay_g": _float(fields[3], "ay_g"),
        "az_g": _float(fields[4], "az_g"),
        "gx_dps": _float(fields[5], "gx_dps"),
        "gy_dps": _float(fields[6], "gy_dps"),
        "gz_dps": _float(fields[7], "gz_dps"),
    }


def _parse_ppg(line: str) -> dict[str, object]:
    fields = _split(line, 4)
    return {
        "t_ms": _uint(fields[1], "t_ms"),
        "red": _uint(fields[2], "red"),
        "ir": _uint(fields[3], "ir"),
    }


def _parse_motion(line: str) -> dict[str, object]:
    fields = _split(line, 11)
    return {
        "t_ms": _uint(fields[2], "t_ms"),
        "quality": _enum(fields[3], QUALITY_NAMES, "quality"),
        "enmo_g": _float(fields[4], "enmo_g"),
        "mag_std_g": _float(fields[5], "mag_std_g"),
        "jerk_rms_g_per_s": _float(fields[6], "jerk_rms_g_per_s"),
        "gyro_rms_dps": _float(fields[7], "gyro_rms_dps"),
        "gravity_tilt_deg": _float(fields[8], "gravity_tilt_deg"),
        "dominant_freq_hz": _float(fields[9], "dominant_freq_hz"),
        "stillness_ms": _uint(fields[10], "stillness_ms"),
    }


def _parse_ppg_features(line: str) -> dict[str, object]:
    fields = _split(line, 13)
    return {
        "t_ms": _uint(fields[2], "t_ms"),
        "quality": _enum(fields[3], QUALITY_NAMES, "quality"),
        "hr_valid": _bool01(fields[4], "hr_valid"),
        "hr_bpm": _float(fields[5], "hr_bpm"),
        "hr_confidence": _float(fields[6], "hr_confidence"),
        "perfusion_index": _float(fields[7], "perfusion_index"),
        "ir_dc_level": _float(fields[8], "ir_dc_level"),
        "sqi": _float(fields[9], "sqi"),
        "motion_corrupted": _bool01(fields[10], "motion_corrupted"),
        "hrv_valid": _bool01(fields[11], "hrv_valid"),
        "rmssd_ms": _float(fields[12], "rmssd_ms"),
    }


def _parse_context(line: str) -> dict[str, object]:
    fields = _split(line, 5)
    return {
        "t_ms": _uint(fields[2], "t_ms"),
        "activity": _enum(fields[3], ACTIVITY_NAMES, "activity"),
        "confidence": _float(fields[4], "confidence"),
    }


def _parse_event(line: str) -> dict[str, object]:
    fields = _split(line, 17)
    payload_hex = fields[8]
    if len(payload_hex) != 16 or any(c not in "0123456789abcdef" for c in payload_hex):
        raise MalformedRow(f"payload_hex must be 16 lower-case hex chars: {payload_hex!r}")
    return {
        "seq": _uint(fields[1], "seq"),
        "t_start_ms": _uint(fields[2], "t_start_ms"),
        "boot_id": _uint(fields[3], "boot_id"),
        "duration_ms": _uint(fields[4], "duration_ms"),
        "t_start_unix": _uint(fields[5], "t_start_unix"),
        "confidence": _float(fields[6], "confidence"),
        "reasons": _uint(fields[7], "reasons"),
        "payload_hex": payload_hex,
        "quality_imu": _enum(fields[9], QUALITY_NAMES, "quality_imu"),
        "quality_ppg": _enum(fields[10], QUALITY_NAMES, "quality_ppg"),
        "type": _enum(fields[11], EVENT_TYPE_NAMES, "type"),
        "label": _enum(fields[12], USER_LABEL_NAMES, "label"),
        "activity": _enum(fields[13], ACTIVITY_NAMES, "activity"),
        "alertness": _enum(fields[14], ALERTNESS_NAMES, "alertness"),
        "detector_id": _uint(fields[15], "detector_id"),
        "schema_version": _uint(fields[16], "schema_version"),
    }


def _parse_marker(line: str) -> dict[str, object]:
    fields = _split(line, 3)
    if not fields[2]:
        raise MalformedRow("marker text is empty")
    return {"t_ms": _uint(fields[1], "t_ms"), "marker": fields[2]}


_PARSERS = {
    "#HDR": ("header", _parse_header),
    "I": ("imu", _parse_imu),
    "P": ("ppg", _parse_ppg),
    "F,M": ("motion", _parse_motion),
    "F,P": ("ppg_features", _parse_ppg_features),
    "F,C": ("context", _parse_context),
    "E": ("event", _parse_event),
    "M": ("marker", _parse_marker),
}


def parse_line(line: str) -> tuple[str, dict[str, object]]:
    """Parse one research line. Raises MalformedRow/SchemaMismatch."""
    stripped = line.rstrip("\r\n")
    if stripped.startswith("#HDR,"):
        return _PARSERS["#HDR"][0], _parse_header(stripped)
    key = stripped[:3] if stripped.startswith("F,") else stripped[:1]
    entry = _PARSERS.get(key)
    if entry is None:
        raise MalformedRow(f"unrecognised research row: {stripped[:60]!r}")
    kind, parser = entry
    return kind, parser(stripped)
