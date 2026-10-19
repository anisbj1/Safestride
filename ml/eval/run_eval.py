"""Headless evaluation harness (F29, spec §7, §15).

Usage::

    python ml/eval/run_eval.py --sessions <dir>... --out <dir> [--detector <id>]
                               [--tolerance-s <float>]

Each session directory is F28 format (DATASET_FORMAT schema v1); predictions
come from ``predictions_<detector_id>.csv`` inside the session directory
(F29 D6 — the ``t_start_ms,duration_ms,confidence`` subset of the F28
``events.csv`` columns; extra columns are tolerated, F29 parses only what it
needs and never re-implements the full DATASET_FORMAT grammar).

Exit codes mirror ``tools/replay/compare.py`` (DATASET_FORMAT §4):
``K_EXIT_OK`` = 0, ``K_EXIT_INPUT_ERROR`` = 2 (missing/unreadable input),
``K_EXIT_SCHEMA_VERSION`` = 4 (unknown ``schema_version``).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

import metrics
import report as report_mod
import split as split_mod
from event_matching import K_EVENT_MATCH_TOLERANCE_S, Event, match_events

# [DERIVED] Exit codes mirror tools/replay/compare.py per DATASET_FORMAT §4
# so every host tool fails identically (spec §8).
K_EXIT_OK = 0
K_EXIT_INPUT_ERROR = 2
K_EXIT_SCHEMA_VERSION = 4

# [DERIVED] The only dataset schema version this skeleton understands
# (DATASET_FORMAT §5: an unknown schema_version refuses the input, exit 4).
_K_SUPPORTED_SCHEMA_VERSION = 1

_K_PREDICTIONS_PREFIX = "predictions_"
_K_PREDICTIONS_SUFFIX = ".csv"
_K_REQUIRED_EVENT_COLUMNS = ("t_start_ms", "duration_ms", "confidence", "label")
_K_REQUIRED_PREDICTION_COLUMNS = ("t_start_ms", "duration_ms", "confidence")
_K_LABEL_CONFIRMED = "Confirmed"
_K_LABEL_REJECTED = "Rejected"
_K_LABEL_UNLABELED = "Unlabeled"
_K_UNLABELED_LABELS = (_K_LABEL_CONFIRMED, _K_LABEL_REJECTED, _K_LABEL_UNLABELED)


class SessionInputError(Exception):
    """A session directory or its files are missing/unreadable/malformed."""


class SchemaVersionError(Exception):
    """The session declares a dataset schema version this tool does not know."""


@dataclass
class SessionData:
    """Parsed ground truth of one F28 session directory."""

    session_id: str
    duration_s: float
    confirmed_labels: list[Event]
    rejected_labels: list[Event]
    unlabeled_s: float
    raw_drops: int
    line_drops: int
    malformed_rows: int


def _parse_utc(value: Any) -> datetime | None:
    if not isinstance(value, str):
        return None
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        return None


def _max_row_time_ms(csv_path: Path) -> int:
    """Largest timestamp found in a session CSV (duration fallback); 0 if none."""
    if not csv_path.exists():
        return 0
    max_ms = 0
    try:
        with csv_path.open(newline="", encoding="utf-8") as file_obj:
            for row in csv.DictReader(file_obj):
                for key in ("t_ms", "t_start_ms"):
                    value = row.get(key) or ""
                    if value.isdigit():
                        max_ms = max(max_ms, int(value))
    except OSError as exc:
        raise SessionInputError(f"unreadable {csv_path}: {exc}") from exc
    return max_ms


def _load_events_csv(csv_path: Path, session: SessionData) -> None:
    if not csv_path.exists():
        return  # an events-free capture has no ground truth; coverage reports it
    try:
        with csv_path.open(newline="", encoding="utf-8") as file_obj:
            reader = csv.DictReader(file_obj)
            header = reader.fieldnames or []
            missing = [c for c in _K_REQUIRED_EVENT_COLUMNS if c not in header]
            if missing:
                raise SessionInputError(f"{csv_path}: missing columns {missing}")
            for row in reader:
                label = (row.get("label") or "").strip()
                if label not in _K_UNLABELED_LABELS:
                    # DATASET_FORMAT §4: an unknown enum name means schema
                    # drift — count the row malformed, never guess it.
                    session.malformed_rows += 1
                    continue
                try:
                    event = Event(
                        t_start_ms=int(row["t_start_ms"]),
                        duration_ms=int(row["duration_ms"]),
                        confidence=float(row["confidence"]),
                    )
                except (TypeError, ValueError):
                    session.malformed_rows += 1
                    continue
                if label == _K_LABEL_CONFIRMED:
                    session.confirmed_labels.append(event)
                elif label == _K_LABEL_REJECTED:
                    session.rejected_labels.append(event)
                else:
                    session.unlabeled_s += event.duration_ms / 1000.0
    except OSError as exc:
        raise SessionInputError(f"unreadable {csv_path}: {exc}") from exc


def load_session(session_dir: Path) -> SessionData:
    """Parse one F28 session directory (session.json + events.csv)."""
    session_json_path = session_dir / "session.json"
    try:
        raw = json.loads(session_json_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise SessionInputError(f"missing {session_json_path}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise SessionInputError(f"unreadable {session_json_path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise SessionInputError(f"{session_json_path}: not a JSON object")

    schema_version = raw.get("schema_version")
    if schema_version != _K_SUPPORTED_SCHEMA_VERSION:
        raise SchemaVersionError(
            f"{session_json_path}: unknown schema_version {schema_version!r} "
            f"(supported: {_K_SUPPORTED_SCHEMA_VERSION})"
        )
    session_id = raw.get("session_id")
    if not isinstance(session_id, str) or not session_id:
        raise SessionInputError(f"{session_json_path}: missing session_id")

    session = SessionData(
        session_id=session_id,
        duration_s=0.0,
        confirmed_labels=[],
        rejected_labels=[],
        unlabeled_s=0.0,
        raw_drops=int(raw.get("raw_drops", 0)),
        line_drops=int(raw.get("line_drops", 0)),
        malformed_rows=int(raw.get("malformed_rows", 0)),
    )
    _load_events_csv(session_dir / "events.csv", session)

    started = _parse_utc(raw.get("started_utc"))
    ended = _parse_utc(raw.get("ended_utc"))
    if started is not None and ended is not None and ended > started:
        session.duration_s = (ended - started).total_seconds()
    else:  # fallback: last recorded row timestamp (never silently 0-with-reason)
        fallback_ms = max(
            _max_row_time_ms(session_dir / "features.csv"),
            _max_row_time_ms(session_dir / "events.csv"),
        )
        session.duration_s = fallback_ms / 1000.0
    return session


def load_predictions(csv_path: Path) -> list[Event]:
    """Parse a ``predictions_<detector_id>.csv`` file (D6 subset, extras tolerated)."""
    try:
        with csv_path.open(newline="", encoding="utf-8") as file_obj:
            reader = csv.DictReader(file_obj)
            header = reader.fieldnames or []
            missing = [c for c in _K_REQUIRED_PREDICTION_COLUMNS if c not in header]
            if missing:
                raise SessionInputError(f"{csv_path}: missing columns {missing}")
            predictions = []
            for line_number, row in enumerate(reader, start=2):
                try:
                    predictions.append(
                        Event(
                            t_start_ms=int(row["t_start_ms"]),
                            duration_ms=int(row["duration_ms"]),
                            confidence=float(row["confidence"]),
                        )
                    )
                except (TypeError, ValueError) as exc:
                    raise SessionInputError(
                        f"{csv_path}:{line_number}: malformed prediction row: {exc}"
                    ) from exc
            return predictions
    except OSError as exc:
        raise SessionInputError(f"unreadable {csv_path}: {exc}") from exc


def _discover_detector_ids(session_dirs: list[Path]) -> list[str]:
    detector_ids: set[str] = set()
    for session_dir in session_dirs:
        for path in session_dir.glob(f"{_K_PREDICTIONS_PREFIX}*{_K_PREDICTIONS_SUFFIX}"):
            detector_ids.add(path.stem[len(_K_PREDICTIONS_PREFIX) :])
    return sorted(detector_ids)


def _nearest_label_dt_s(prediction: Event, labels: list[Event]) -> float | None:
    """prediction − nearest label (s); ``None`` when there is no label at all."""
    if not labels:
        return None
    return min((prediction.t_start_ms - label.t_start_ms for label in labels), key=abs) / 1000.0


def _nearest_prediction_dt_s(label: Event, predictions: list[Event]) -> float | None:
    """nearest prediction − label (s); ``None`` when the detector never fired."""
    if not predictions:
        return None
    return min((p.t_start_ms - label.t_start_ms for p in predictions), key=abs) / 1000.0


def _false_positive_entries(
    session: SessionData, predictions: list[Event], fp_indices: tuple[int, ...]
) -> list[dict[str, Any]]:
    labeled = session.confirmed_labels + session.rejected_labels
    return [
        report_mod.fp_entry(
            session=session.session_id,
            t_start_ms=predictions[i].t_start_ms,
            confidence=predictions[i].confidence,
            nearest_label_dt_s=_nearest_label_dt_s(predictions[i], labeled),
        )
        for i in fp_indices
    ]


def _false_negative_entries(
    session: SessionData, predictions: list[Event], fn_indices: tuple[int, ...]
) -> list[dict[str, Any]]:
    return [
        report_mod.fn_entry(
            session=session.session_id,
            t_start_ms=session.confirmed_labels[i].t_start_ms,
            confidence=session.confirmed_labels[i].confidence,
            nearest_prediction_dt_s=_nearest_prediction_dt_s(
                session.confirmed_labels[i], predictions
            ),
        )
        for i in fn_indices
    ]


def evaluate(
    session_dirs: list[Path],
    detector_id: str | None = None,
    tolerance_s: float = float(K_EVENT_MATCH_TOLERANCE_S),
) -> dict[str, Any]:
    """Run leave-one-session-out event evaluation over F28 session directories."""
    if not session_dirs:
        raise SessionInputError("no session directories given")
    sessions = [load_session(session_dir) for session_dir in session_dirs]

    detector_ids = _discover_detector_ids(session_dirs)
    if not detector_ids:
        raise SessionInputError(
            f"no {_K_PREDICTIONS_PREFIX}<detector_id>{_K_PREDICTIONS_SUFFIX} files "
            "found in any session directory"
        )
    if detector_id is None:
        if len(detector_ids) > 1:
            raise SessionInputError(
                f"multiple detectors found {detector_ids}; select one with --detector "
                "(one report per detector in the M2 skeleton; comparison[] is where "
                "multi-detector merges land in M5)"
            )
        detector_id = detector_ids[0]
    elif detector_id not in detector_ids:
        raise SessionInputError(f"detector {detector_id!r} not found; have {detector_ids}")

    predictions_by_session: dict[str, list[Event]] = {}
    missing_prediction_files: list[str] = []
    for session_dir, session in zip(session_dirs, sessions, strict=True):
        predictions_path = (
            session_dir / f"{_K_PREDICTIONS_PREFIX}{detector_id}{_K_PREDICTIONS_SUFFIX}"
        )
        if predictions_path.exists():
            predictions_by_session[session.session_id] = load_predictions(predictions_path)
        else:  # never silent: a missing file must not look like "no false alerts"
            predictions_by_session[session.session_id] = []
            missing_prediction_files.append(f"{session.session_id}:{detector_id}")

    try:
        fold_specs = split_mod.leave_one_session_out([s.session_id for s in sessions])
    except ValueError as exc:
        # These ValueErrors describe bad *inputs* (<2 sessions, duplicate
        # ids), not internal bugs: surface them through the documented
        # K_EXIT_INPUT_ERROR path (exit 2) instead of an unhandled traceback.
        raise SessionInputError(str(exc)) from exc

    folds: list[dict[str, Any]] = []
    fold_blocks: list[dict[str, Any]] = []
    aggregate_tp_fp_fn = (0, 0, 0)
    aggregate_dt: list[float] = []
    aggregate_fp: list[dict[str, Any]] = []
    aggregate_fn: list[dict[str, Any]] = []
    per_session_notes: dict[str, Any] = {}
    unlabeled_s_total = 0.0
    total_duration_s = 0.0

    for fold in fold_specs:
        session = next(s for s in sessions if s.session_id == fold.test_session)
        predictions = predictions_by_session[session.session_id]
        match_result = match_events(session.confirmed_labels, predictions, tolerance_s)
        block_metrics = metrics.event_level_metrics(
            tp=match_result.tp,
            fp=match_result.fp,
            fn=match_result.fn,
            matched_dt_s=match_result.matched_dt_s,
            total_duration_s=session.duration_s,
            unlabeled_s=session.unlabeled_s,
        )
        fold_blocks.append(
            report_mod.metric_block(
                block_metrics,
                false_positives=_false_positive_entries(
                    session, predictions, match_result.false_positive_indices
                ),
                false_negatives=_false_negative_entries(
                    session, predictions, match_result.false_negative_indices
                ),
            )
        )
        folds.append(
            report_mod.fold_entry(
                fold_index=fold.fold_index,
                test_session=fold.test_session,
                detector_id=detector_id,
                block=fold_blocks[-1],
            )
        )
        aggregate_tp_fp_fn = metrics.add_counts(
            aggregate_tp_fp_fn, (match_result.tp, match_result.fp, match_result.fn)
        )
        aggregate_dt.extend(match_result.matched_dt_s)
        aggregate_fp.extend(fold_blocks[-1]["false_positives"])
        aggregate_fn.extend(fold_blocks[-1]["false_negatives"])

    for session in sessions:
        total_duration_s += session.duration_s
        unlabeled_s_total += session.unlabeled_s
        per_session_notes[session.session_id] = {
            "duration_s": session.duration_s,
            "unlabeled_s": session.unlabeled_s,
            "unlabeled_coverage": metrics.unlabeled_coverage(
                session.unlabeled_s, session.duration_s
            ),
            "raw_drops": session.raw_drops,
            "line_drops": session.line_drops,
            "malformed_rows": session.malformed_rows,
        }

    aggregate_metrics = metrics.event_level_metrics(
        tp=aggregate_tp_fp_fn[0],
        fp=aggregate_tp_fp_fn[1],
        fn=aggregate_tp_fp_fn[2],
        matched_dt_s=aggregate_dt,
        total_duration_s=total_duration_s,
        unlabeled_s=unlabeled_s_total,
    )
    aggregate_block = report_mod.metric_block(
        aggregate_metrics, false_positives=aggregate_fp, false_negatives=aggregate_fn
    )
    dataset_notes = {
        "raw_drops": sum(s.raw_drops for s in sessions),
        "line_drops": sum(s.line_drops for s in sessions),
        "malformed_rows": sum(s.malformed_rows for s in sessions),
        "missing_prediction_files": missing_prediction_files,
        "unlabeled_s_total": unlabeled_s_total,
        "per_session": per_session_notes,
    }
    return report_mod.build_report(
        detector_id=detector_id,
        tolerance_s=tolerance_s,
        session_ids=[s.session_id for s in sessions],
        total_duration_s=total_duration_s,
        unlabeled_coverage=aggregate_metrics.unlabeled_coverage,
        dataset_notes=dataset_notes,
        folds=folds,
        aggregate_block=aggregate_block,
        comparison=[report_mod.comparison_row(detector_id=detector_id, block=aggregate_block)],
    )


def write_report(report: dict[str, Any], out_dir: Path) -> None:
    """Write ``report.json`` + ``report.md`` (LF newlines, deterministic bytes)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "report.json").write_text(report_mod.to_json(report), encoding="utf-8", newline="")
    (out_dir / "report.md").write_text(
        report_mod.render_markdown(report), encoding="utf-8", newline=""
    )


def _tolerance_s_arg(value: str) -> float:
    """argparse type: reject a non-finite or non-positive match tolerance.

    A tolerance of 0 or less would make matching meaningless (dt ≤ tolerance
    would never/always match depending on sign), so it is a usage error. An
    infinite tolerance is likewise nonsense: it would match every prediction to
    every label, silently inflating TP/recall (spec §15: "> 0", 00 §7.5: never
    fabricate confidence from bad data).
    """
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{value!r} is not a number") from exc
    if not math.isfinite(parsed):  # also rejects NaN, but NaN is caught below too
        raise argparse.ArgumentTypeError(f"tolerance must be finite, got {value!r}")
    if not parsed > 0:  # NaN > 0 is False, so NaN lands here as well
        raise argparse.ArgumentTypeError(f"tolerance must be > 0, got {value!r}")
    return parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="run_eval.py",
        description="F29 event-level evaluation over F28 session directories (not a diagnosis).",
    )
    parser.add_argument("--sessions", nargs="+", required=True, help="F28 session directories")
    parser.add_argument("--out", required=True, help="output directory for report.json/report.md")
    parser.add_argument(
        "--detector", default=None, help="detector id (default: the only one found)"
    )
    parser.add_argument(
        "--tolerance-s",
        type=_tolerance_s_arg,
        default=float(K_EVENT_MATCH_TOLERANCE_S),
        help="event match tolerance in seconds (finite, > 0; default: the spec constant)",
    )
    args = parser.parse_args(argv)
    try:
        evaluation = evaluate(
            [Path(p) for p in args.sessions],
            detector_id=args.detector,
            tolerance_s=args.tolerance_s,
        )
    except SchemaVersionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return K_EXIT_SCHEMA_VERSION
    except SessionInputError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return K_EXIT_INPUT_ERROR
    write_report(evaluation, Path(args.out))
    return K_EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
