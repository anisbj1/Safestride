"""Deterministic F28-format synthetic session builder + stub detector (F29 D3).

No RNG anywhere: sessions are hand-written timelines, so regenerating them is
byte-identical — this is what makes the committed example report under
``docs/evaluation/example/`` drift-guardable (D3, the F28 CR-012 lesson).

The written files mirror what ``tools/capture`` writes (header row first,
same column names as the DATASET_FORMAT §2.1 grammar): ``session.json``,
``features.csv`` (motion rows), ``events.csv`` (``Confirmed`` / ``Rejected`` /
``Unlabeled`` labels). The stub detector is the trivial stillness-threshold
baseline: it fires on the first motion row whose ``stillness_ms`` exceeds a
named threshold, with a refractory window so each burst yields one firing.
"""

from __future__ import annotations

import csv
import json
from datetime import datetime, timedelta
from pathlib import Path

from event_matching import Event

# [EXPERIMENTAL] Stub fires above this stillness; arbitrary but named and
# pinned by tests (spec §8).
K_STUB_STILLNESS_THRESHOLD_MS = 45000

# [EXPERIMENTAL] Stub suppresses re-fires within this window so each synthetic
# burst yields one prediction (spec §8).
K_STUB_REFRACTORY_S = 120

# [EXPERIMENTAL] Fixed neutral confidence: the stub has no signal-derived
# evidence to report, so it always claims the same low confidence.
K_STUB_CONFIDENCE = 0.5

# [EXPERIMENTAL] Ground-truth confidence written on synthetic labels; a fixed
# value keeps fixtures deterministic (formatting pinned to %.3f, DATASET §2).
K_LABEL_CONFIDENCE = 0.9

# [DERIVED] F28 emits 1 Hz feature rows (DATASET_FORMAT §2.1: "the 1 Hz
# feature rows"), so the fixture writes one motion row per second.
_K_FEATURE_PERIOD_MS = 1000

# [EXPERIMENTAL] Burst rows exceed the stub threshold with margin so the fire
# time is exactly the first burst row; 5 s keeps bursts multi-row but far
# below the refractory window.
_K_BURST_STILLNESS_MS = 50000
_K_BURST_LENGTH_S = 5

# Column lists mirror tools/capture/session.py (DATASET_FORMAT §3); the
# fixture must stay a faithful F28 session directory.
FEATURES_COLUMNS = [
    "t_ms", "boot_id", "kind", "quality", "enmo_g", "mag_std_g",
    "jerk_rms_g_per_s", "gyro_rms_dps", "gravity_tilt_deg", "dominant_freq_hz",
    "stillness_ms", "hr_valid", "hr_bpm", "hr_confidence", "perfusion_index",
    "ir_dc_level", "sqi", "motion_corrupted", "hrv_valid", "rmssd_ms",
    "activity", "confidence",
]
EVENTS_COLUMNS = [
    "seq", "t_start_ms", "boot_id", "duration_ms", "t_start_unix", "confidence",
    "reasons", "payload_hex", "quality_imu", "quality_ppg", "type", "label",
    "activity", "alertness", "detector_id", "schema_version",
]
PREDICTIONS_COLUMNS = ["t_start_ms", "duration_ms", "confidence"]

# Motion-row constants for synthetic stillness: a body at rest has small
# acceleration features; the values only need to be plausible and constant.
_K_STILL_MOTION = {
    "enmo_g": "0.0100",
    "mag_std_g": "0.0200",
    "jerk_rms_g_per_s": "0.0300",
    "gyro_rms_dps": "1.000",
    "gravity_tilt_deg": "45.0",
    "dominant_freq_hz": "1.000",
}


def _write_csv(path: Path, columns: list[str], rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as file_obj:
        writer = csv.writer(file_obj, lineterminator="\n")
        writer.writerow(columns)
        for row in rows:
            writer.writerow([row.get(column, "") for column in columns])


def write_session(
    root: Path,
    *,
    session_id: str,
    started_utc: str,
    duration_s: int,
    confirmed_labels_s: tuple[int, ...],
    rejected_labels_s: tuple[int, ...],
    unlabeled_windows_s: tuple[tuple[int, int], ...],
    bursts_s: tuple[int, ...],
) -> Path:
    """Write one deterministic F28-format session directory; returns its path."""
    session_dir = root / session_id
    session_dir.mkdir(parents=True, exist_ok=True)
    started_dt = datetime.fromisoformat(started_utc)  # fixed input → deterministic output
    ended = (started_dt + timedelta(seconds=duration_s)).strftime("%Y-%m-%dT%H:%M:%SZ")

    burst_starts = set()
    for burst_s in bursts_s:
        burst_starts.update(range(burst_s, burst_s + _K_BURST_LENGTH_S))

    feature_rows: list[dict[str, object]] = []
    for second in range(duration_s):
        stillness = _K_BURST_STILLNESS_MS if second in burst_starts else 0
        row: dict[str, object] = {
            "t_ms": second * _K_FEATURE_PERIOD_MS,
            "boot_id": 0,
            "kind": "motion",
            "quality": "Good",
            "stillness_ms": stillness,
        }
        row.update(_K_STILL_MOTION)
        feature_rows.append(row)

    event_rows: list[dict[str, object]] = []

    def _event_row(
        seq: int, t_s: int, duration_s_row: int, label: str, confidence: str
    ) -> dict[str, object]:
        return {
            "seq": seq,
            "t_start_ms": t_s * 1000,
            "boot_id": 0,
            "duration_ms": duration_s_row * 1000,
            "t_start_unix": 0,
            "confidence": confidence,
            "reasons": 0,
            "payload_hex": "0000000000000000",
            "quality_imu": "Good",
            "quality_ppg": "Good",
            "type": "UserMarked",
            "label": label,
            "activity": "Still",
            "alertness": "Awake",
            "detector_id": "synthetic_gt",
            "schema_version": 1,
        }

    seq = 0
    for t_s in confirmed_labels_s:
        seq += 1
        event_rows.append(_event_row(seq, t_s, 0, "Confirmed", f"{K_LABEL_CONFIDENCE:.3f}"))
    for t_s in rejected_labels_s:
        seq += 1
        event_rows.append(_event_row(seq, t_s, 0, "Rejected", f"{K_LABEL_CONFIDENCE:.3f}"))
    for t_s, window_s in unlabeled_windows_s:
        seq += 1
        # Unknown truth: excluded from matching, but its duration feeds the
        # unlabeled-coverage report (spec §10: missing labels, don't guess).
        event_rows.append(_event_row(seq, t_s, window_s, "Unlabeled", "0.000"))

    session_json = {
        "schema_version": 1,
        "session_id": session_id,
        "boot_id": 0,
        "firmware_version": "0.1.0",
        "started_utc": started_utc,
        "ended_utc": ended,
        "end_reason": "user",
        "row_counts": {
            "header": 0,
            "imu": 0,
            "ppg": 0,
            "motion": len(feature_rows),
            "ppg_features": 0,
            "context": 0,
            "event": len(event_rows),
            "marker": 0,
        },
        "raw_drops": 0,
        "line_drops": 0,
        "malformed_rows": 0,
        "diagnostics_lines": 0,
        "raw_tee": False,
        "counters_as_of_hdr_seconds": duration_s,
    }
    (session_dir / "session.json").write_text(
        json.dumps(session_json, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline=""
    )
    _write_csv(session_dir / "features.csv", FEATURES_COLUMNS, feature_rows)
    _write_csv(session_dir / "events.csv", EVENTS_COLUMNS, event_rows)
    return session_dir


def run_stub_detector(session_dir: Path, detector_id: str = "stub_stillness") -> list[Event]:
    """Stillness-threshold stub: fire where motion rows exceed the threshold.

    Writes ``predictions_<detector_id>.csv`` (F29 D6 subset) into the session
    directory and returns the same predictions.
    """
    predictions: list[Event] = []
    last_fire_ms: int | None = None
    with (session_dir / "features.csv").open(newline="", encoding="utf-8") as file_obj:
        for row in csv.DictReader(file_obj):
            if row.get("kind") != "motion" or not (row.get("stillness_ms") or "").isdigit():
                continue
            stillness_ms = int(row["stillness_ms"])
            t_ms = int(row["t_ms"])
            if stillness_ms <= K_STUB_STILLNESS_THRESHOLD_MS:
                continue
            if last_fire_ms is not None and t_ms - last_fire_ms < K_STUB_REFRACTORY_S * 1000:
                continue
            predictions.append(Event(t_start_ms=t_ms, duration_ms=0, confidence=K_STUB_CONFIDENCE))
            last_fire_ms = t_ms

    with (session_dir / f"predictions_{detector_id}.csv").open(
        "w", newline="", encoding="utf-8"
    ) as file_obj:
        writer = csv.writer(file_obj, lineterminator="\n")
        writer.writerow(PREDICTIONS_COLUMNS)
        for prediction in predictions:
            writer.writerow(
                [prediction.t_start_ms, prediction.duration_ms, f"{prediction.confidence:.3f}"]
            )
    return predictions


def write_canonical_sessions(root: Path) -> tuple[list[Path], str]:
    """The hand-computed two-session set of spec §9/e2e; shared by tests and example."""
    session_a = write_session(
        root,
        session_id="synthetic_sA",
        started_utc="2026-01-01T00:00:00Z",
        duration_s=7200,
        confirmed_labels_s=(1200, 3600, 5800),
        rejected_labels_s=(2400,),
        unlabeled_windows_s=((500, 600),),
        bursts_s=(1220, 3580, 5850, 2410),
    )
    session_b = write_session(
        root,
        session_id="synthetic_sB",
        started_utc="2026-01-02T00:00:00Z",
        duration_s=5400,
        confirmed_labels_s=(1000, 4000),
        rejected_labels_s=(),
        unlabeled_windows_s=(),
        bursts_s=(1030, 4050),
    )
    for session_dir in (session_a, session_b):
        run_stub_detector(session_dir)
    return [session_a, session_b], "stub_stillness"
