"""Session directory writer (DATASET_FORMAT.md §3): CSVs + session.json.

One `SessionManager` owns the capture: it classifies each incoming line,
routes research rows to their CSV, diverts everything else to
`diagnostics.log`, counts losses (malformed rows, drop counters from #HDR)
and finalizes `session.json` — including the end reason, so a reader can
tell a completed session from an interrupted one.
"""

from __future__ import annotations

import csv
import json
import time
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path

from capture import lines as linemod

RAW_IMU_COLUMNS = [
    "t_ms",
    "boot_id",
    "ax_g",
    "ay_g",
    "az_g",
    "gx_dps",
    "gy_dps",
    "gz_dps",
]
RAW_PPG_COLUMNS = ["t_ms", "boot_id", "red", "ir"]
FEATURES_COLUMNS = [
    "t_ms",
    "boot_id",
    "kind",
    "quality",
    "enmo_g",
    "mag_std_g",
    "jerk_rms_g_per_s",
    "gyro_rms_dps",
    "gravity_tilt_deg",
    "dominant_freq_hz",
    "stillness_ms",
    "hr_valid",
    "hr_bpm",
    "hr_confidence",
    "perfusion_index",
    "ir_dc_level",
    "sqi",
    "motion_corrupted",
    "hrv_valid",
    "rmssd_ms",
    "activity",
    "confidence",
]
EVENTS_COLUMNS = [
    "seq",
    "t_start_ms",
    "boot_id",
    "duration_ms",
    "t_start_unix",
    "confidence",
    "reasons",
    "payload_hex",
    "quality_imu",
    "quality_ppg",
    "type",
    "label",
    "activity",
    "alertness",
    "detector_id",
    "schema_version",
]
MARKERS_COLUMNS = ["t_ms", "boot_id", "marker", "source"]

ROW_KINDS = ("header", "imu", "ppg", "motion", "ppg_features", "context", "event", "marker")

_END_REASONS = ("user", "schema_change", "boot_change", "eof")


def _csv_cell(value: object) -> object:
    """DATASET_FORMAT.md §5: booleans serialize as "0"/"1", never True/False."""
    if isinstance(value, bool):
        return "1" if value else "0"
    return value


class _Session:
    """One session directory: lazily created CSVs plus the counters."""

    def __init__(self, root: Path, session_id: str) -> None:
        self.dir = root / session_id
        self.dir.mkdir(parents=True)
        self.started_utc = datetime.now(UTC)
        self.started_monotonic = time.monotonic()
        self.boot_id = 0
        self.firmware_version = "unknown"
        self.row_counts = {kind: 0 for kind in ROW_KINDS}
        self.malformed_rows = 0
        self.diagnostics_lines = 0
        self.raw_drops = 0
        self.line_drops = 0
        # Per-source counters from the last #HDR (DATASET_FORMAT §3): a
        # self-describing session dir records the split, not just the sum.
        self.imu_drops = 0
        self.ppg_drops = 0
        self.feature_missed = 0
        # One sample per #HDR received, so a drop curve survives the capture
        # (F28 FIX 2026-10-13: previously only the last header's counters did).
        self.drop_history: list[dict[str, int]] = []
        self.counters_as_of_hdr_seconds: int | None = None
        self._files: dict[str, tuple[object, object]] = {}
        self._diagnostics = (self.dir / "diagnostics.log").open("w", encoding="utf-8")
        self._headers = (self.dir / "headers.log").open("w", encoding="utf-8")
        self._imu_times: list[int] = []
        self._ppg_times: list[int] = []
        # Set by finalize(): host-side gap cross-check (DATASET_FORMAT §3).
        self.imu_period_ms: int | None = None
        self.imu_missing_samples: int | None = None
        self.ppg_period_ms: int | None = None
        self.ppg_missing_samples: int | None = None

    # -- row routing ---------------------------------------------------------
    def write(self, kind: str, row: dict[str, object]) -> None:
        self.row_counts[kind] += 1
        if kind == "event":
            self._write_csv("events", EVENTS_COLUMNS, row)
        elif kind == "marker":
            marker_row = {
                "t_ms": row["t_ms"],
                "boot_id": self.boot_id,
                "marker": row["marker"],
                "source": "device",
            }
            self._write_csv("markers", MARKERS_COLUMNS, marker_row)
        elif kind == "motion":
            self._write_feature("motion", row)
        elif kind == "ppg_features":
            self._write_feature("ppg", row)
        elif kind == "context":
            self._write_feature("context", row)
        elif kind == "imu":
            out = {"t_ms": row["t_ms"], "boot_id": self.boot_id}
            out.update(row)
            self._imu_times.append(int(row["t_ms"]))
            self._write_csv("raw_imu", RAW_IMU_COLUMNS, out)
        elif kind == "ppg":
            out = {"t_ms": row["t_ms"], "boot_id": self.boot_id}
            out.update(row)
            self._ppg_times.append(int(row["t_ms"]))
            self._write_csv("raw_ppg", RAW_PPG_COLUMNS, out)

    def write_host_marker(self, t_ms: int, text: str) -> None:
        self.row_counts["marker"] += 1
        self._write_csv(
            "markers",
            MARKERS_COLUMNS,
            {"t_ms": t_ms, "boot_id": self.boot_id, "marker": text, "source": "host"},
        )

    def note_header(self, header: dict[str, object], raw_line: str = "") -> None:
        self.row_counts["header"] += 1
        self.boot_id = int(header["boot_id"])
        self.firmware_version = str(header["firmware_version"])
        self.imu_drops = int(header["imu_drops"])
        self.ppg_drops = int(header["ppg_drops"])
        self.line_drops = int(header["line_drops"])
        self.feature_missed = int(header["feature_missed"])
        self.raw_drops = self.imu_drops + self.ppg_drops + self.line_drops
        at_s = int(time.monotonic() - self.started_monotonic)
        self.counters_as_of_hdr_seconds = at_s
        self.drop_history.append(
            {
                "at_s": at_s,
                "imu_drops": self.imu_drops,
                "ppg_drops": self.ppg_drops,
                "line_drops": self.line_drops,
                "feature_missed": self.feature_missed,
            }
        )
        if raw_line:  # verbatim, so the session dir can be re-read offline
            self._headers.write(raw_line + "\n")

    def add_diagnostic(self, line: str) -> None:
        self.diagnostics_lines += 1
        self._diagnostics.write(line + "\n")

    def add_malformed(self, line: str) -> None:
        self.malformed_rows += 1
        self.add_diagnostic("MALFORMED " + line)

    # -- helpers -------------------------------------------------------------
    def _write_feature(self, name: str, row: dict[str, object]) -> None:
        out = {column: "" for column in FEATURES_COLUMNS}
        out.update(row)
        out["t_ms"] = row["t_ms"]
        out["boot_id"] = self.boot_id
        out["kind"] = name
        self._write_csv("features", FEATURES_COLUMNS, out)

    def _write_csv(self, name: str, columns: list[str], row: dict[str, object]) -> None:
        handle = self._files.get(name)
        if handle is None:
            path = self.dir / f"{name}.csv"
            file_obj = path.open("w", newline="", encoding="utf-8")
            writer = csv.writer(file_obj)
            writer.writerow(columns)
            handle = (file_obj, writer)
            self._files[name] = handle
        _, writer = handle
        # DATASET_FORMAT.md §5: feature booleans are written as "0"/"1" (the
        # F-row grammar). Python's str(bool) ("True"/"False") violated that
        # contract and made host compare treat every ppg boolean as a diff
        # (F28 D12, 2026-10-13 bench session).
        writer.writerow([_csv_cell(row.get(column, "")) for column in columns])

    # -- teardown ------------------------------------------------------------
    def finalize(self, end_reason: str) -> Path:
        assert end_reason in _END_REASONS
        for file_obj, _ in self._files.values():
            file_obj.close()
        self._diagnostics.close()
        self._headers.close()
        ended = datetime.now(UTC)
        raw_rows = self.row_counts["imu"] + self.row_counts["ppg"]
        imu_period, imu_missing = _gap_stats(self._imu_times)
        ppg_period, ppg_missing = _gap_stats(self._ppg_times)
        self.imu_period_ms, self.imu_missing_samples = imu_period, imu_missing
        self.ppg_period_ms, self.ppg_missing_samples = ppg_period, ppg_missing
        session_json = {
            "schema_version": linemod.SCHEMA_VERSION,
            "session_id": self.dir.name,
            "boot_id": self.boot_id,
            "firmware_version": self.firmware_version,
            "started_utc": _iso(self.started_utc),
            "ended_utc": _iso(ended),
            "end_reason": end_reason,
            "row_counts": dict(self.row_counts),
            "raw_drops": self.raw_drops,
            "imu_drops": self.imu_drops,
            "ppg_drops": self.ppg_drops,
            "line_drops": self.line_drops,
            "feature_missed": self.feature_missed,
            "drop_history": list(self.drop_history),
            "imu_period_ms": imu_period,
            "imu_missing_samples": imu_missing,
            "ppg_period_ms": ppg_period,
            "ppg_missing_samples": ppg_missing,
            "malformed_rows": self.malformed_rows,
            "diagnostics_lines": self.diagnostics_lines,
            "raw_tee": raw_rows > 0,
            "counters_as_of_hdr_seconds": self.counters_as_of_hdr_seconds,
        }
        (self.dir / "session.json").write_text(
            json.dumps(session_json, indent=2) + "\n", encoding="utf-8"
        )
        return self.dir


class SessionManager:
    """Feeds serial lines into session directories (rotating on boot_id change)."""

    def __init__(self, out_dir: Path | str) -> None:
        self._root = Path(out_dir)
        self._root.mkdir(parents=True, exist_ok=True)
        self._session: _Session | None = None

    @property
    def session(self) -> _Session | None:
        return self._session

    def _new_session(self) -> _Session:
        base = datetime.now(UTC).strftime("research_%Y%m%dT%H%M%SZ")
        session_id = base
        suffix = 1
        while (self._root / session_id).exists():
            suffix += 1
            session_id = f"{base}_{suffix}"
        self._session = _Session(self._root, session_id)
        return self._session

    def feed_line(self, line: str) -> None:
        """Route one serial line. Raises SchemaMismatch for unknown schemas."""
        stripped = line.rstrip("\r\n")
        if not stripped:
            return
        if not linemod.is_research_line(stripped):
            if self._session is not None:
                self._session.add_diagnostic(stripped)
            return
        try:
            kind, row = linemod.parse_line(stripped)
        except linemod.SchemaMismatch:
            raise
        except linemod.MalformedRow:
            if self._session is not None:
                self._session.add_malformed(stripped)
            return
        if kind == "header":
            self._handle_header(row, stripped)
            return
        if self._session is None:
            self._new_session()  # rows before any #HDR: keep them, boot_id 0
        self._session.write(kind, row)

    def add_host_marker(self, text: str, t_ms: int = 0) -> None:
        if self._session is None:
            self._new_session()
        self._session.write_host_marker(t_ms, text)

    def finalize(self, end_reason: str) -> Path:
        if self._session is None:
            self._new_session()  # empty capture: still a directory, zero counts
        return self._session.finalize(end_reason)

    def _handle_header(self, header: dict[str, object], raw_line: str) -> None:
        boot_id = int(header["boot_id"])
        if self._session is None:
            self._new_session()
            self._session.note_header(header, raw_line)
            return
        if boot_id != self._session.boot_id and self._session.row_counts["header"] > 0:
            # Device rebooted mid-capture: close the old session honestly and
            # start a new one (DATASET_FORMAT.md §4).
            self._session.finalize("boot_change")
            self._new_session()
        self._session.note_header(header, raw_line)


def _gap_stats(times: list[int]) -> tuple[int | None, int | None]:
    """Host-side loss cross-check (F28 FIX, DATASET_FORMAT §3).

    Returns ``(period_ms, missing_samples)`` derived ONLY from the captured
    ``t_ms`` sequence: the modal positive delta is the device sample period
    ``[DERIVED]`` (robust to ±1 ms rounding jitter), and every gap spanning k
    periods hides k-1 sample slots. Independent of the device drop counters,
    so a session dir stays self-describing even if the device counters are
    absent or wrong — never guessed, never repaired (F28 §10).
    """
    deltas = [b - a for a, b in zip(times, times[1:], strict=False) if b > a]
    if not deltas:
        return None, None
    period = Counter(deltas).most_common(1)[0][0]
    if period <= 0:
        return None, None
    missing = sum(max(0, round(d / period) - 1) for d in deltas)
    return period, missing


def _iso(moment: datetime) -> str:
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")
