"""Evaluation report model and renderers (F29, spec §7, §15 report format design).

The report is a plain JSON-compatible dict (str / int / float / None / list /
dict) so ``json.loads(to_json(r)) == r`` round-trips exactly. The full output
contract — key-by-key schema, markdown section order, determinism guarantees —
is documented in ``docs/evaluation/REPORT_FORMAT.md``.

Rules pinned by the spec and enforced here:
- No ``accuracy`` key anywhere (§5: never accuracy alone).
- Undefined metrics are ``None`` → JSON ``null`` → markdown ``n/a`` (D4).
- False positives and false negatives are always separate sections, rendered
  even when empty (§1, §15).
"""

from __future__ import annotations

import json
from typing import Any

from metrics import EventLevelMetrics

# [DERIVED] Versioned output contract; bump on any report shape change
# (spec §8). A change here must update docs/evaluation/REPORT_FORMAT.md and
# the committed example in the same commit.
K_REPORT_SCHEMA_VERSION = 1

_GENERATED_BY = "F29 ml/eval/run_eval.py (M2 skeleton)"

# Wording rule (00 §1): detections are possible/suspected with a confidence
# value; the project is not a medical device and never diagnoses.
DISCLAIMER = (
    "Detections are possible/suspected events with a confidence value. "
    "This report is evaluation output from an academic prototype — "
    "not a diagnosis, not a medical device."
)

# Fixed markdown section order (spec §15); the drift-guarded example depends
# on this order, so changing it is a report-shape change (schema bump).
_MARKDOWN_SECTIONS = (
    "Configuration",
    "Dataset",
    "Event-level metrics",
    "False positives",
    "False negatives",
    "Detection latency",
    "Unlabeled coverage",
    "Model comparison",
)

_METRIC_TABLE_HEADER = (
    "| tp | fp | fn | precision | recall | f1 "
    "| false_alerts_per_day | missed_event_rate |"
)
_METRIC_TABLE_SEP = "|---|---|---|---|---|---|---|---|"

_COMPARISON_KEYS = (
    "tp",
    "fp",
    "fn",
    "precision",
    "recall",
    "f1",
    "false_alerts_per_day",
    "missed_event_rate",
    "latency",
)


def fp_entry(
    session: str, t_start_ms: int, confidence: float, nearest_label_dt_s: float | None
) -> dict[str, Any]:
    """One false-positive row: a firing with no label inside the tolerance.

    ``nearest_label_dt_s`` (prediction − label, s) is measured against the
    nearest labeled event of any status except ``Unlabeled`` — a ``Rejected``
    label is ground truth too ("not an event"), and showing the distance to it
    is what makes a false positive analysable. ``None`` when the dataset has
    no labeled event at all.
    """
    return {
        "session": session,
        "t_start_ms": t_start_ms,
        "confidence": confidence,
        "nearest_label_dt_s": nearest_label_dt_s,
    }


def fn_entry(
    session: str, t_start_ms: int, confidence: float, nearest_prediction_dt_s: float | None
) -> dict[str, Any]:
    """One false-negative row: a label the detector never fired on."""
    return {
        "session": session,
        "t_start_ms": t_start_ms,
        "confidence": confidence,
        "nearest_prediction_dt_s": nearest_prediction_dt_s,
    }


def metric_block(
    metrics: EventLevelMetrics,
    false_positives: list[dict[str, Any]] | tuple[dict[str, Any], ...] = (),
    false_negatives: list[dict[str, Any]] | tuple[dict[str, Any], ...] = (),
) -> dict[str, Any]:
    """The metric block shared by folds, the aggregate and comparison rows."""
    return {
        "tp": metrics.tp,
        "fp": metrics.fp,
        "fn": metrics.fn,
        "precision": metrics.precision,
        "recall": metrics.recall,
        "f1": metrics.f1,
        "false_alerts_per_day": metrics.false_alerts_per_day,
        "missed_event_rate": metrics.missed_event_rate,
        "latency": {
            "mean_s": metrics.latency.mean_s,
            "median_s": metrics.latency.median_s,
            "min_s": metrics.latency.min_s,
            "max_s": metrics.latency.max_s,
        },
        "false_positives": list(false_positives),
        "false_negatives": list(false_negatives),
    }


def fold_entry(
    *, fold_index: int, test_session: str, detector_id: str, block: dict[str, Any]
) -> dict[str, Any]:
    """One fold of ``folds[]`` (spec §15)."""
    return {
        "fold_index": fold_index,
        "test_session": test_session,
        "detector_id": detector_id,
        **block,
    }


def comparison_row(*, detector_id: str, block: dict[str, Any]) -> dict[str, Any]:
    """One row of ``comparison[]`` — one per detector (M5 compares runs here)."""
    return {"detector_id": detector_id, **{key: block[key] for key in _COMPARISON_KEYS}}


def build_report(
    *,
    detector_id: str,
    tolerance_s: float,
    session_ids: list[str],
    total_duration_s: float,
    unlabeled_coverage: float | None,
    dataset_notes: dict[str, Any],
    folds: list[dict[str, Any]],
    aggregate_block: dict[str, Any],
    comparison: list[dict[str, Any]],
) -> dict[str, Any]:
    """Assemble the top-level report object (shape = REPORT_FORMAT.md schema v1)."""
    return {
        "schema_version": K_REPORT_SCHEMA_VERSION,
        "generated_by": _GENERATED_BY,
        "disclaimer": DISCLAIMER,
        "config": {
            "detector_id": detector_id,
            "event_match_tolerance_s": tolerance_s,
            "split": "leave-one-session-out",
        },
        "dataset": {
            "session_ids": list(session_ids),
            "total_duration_s": total_duration_s,
            "unlabeled_coverage": unlabeled_coverage,
            "notes": dataset_notes,
        },
        "folds": folds,
        "aggregate": aggregate_block,
        "comparison": comparison,
    }


def to_json(report: dict[str, Any]) -> str:
    """Deterministic JSON text: sorted keys, LF, trailing newline."""
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def _fmt_rate(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.3f}"


def _fmt_latency(latency: dict[str, float | None]) -> str:
    return " | ".join(_fmt_rate(latency[key]) for key in ("mean_s", "median_s", "min_s", "max_s"))


def _metric_row(block: dict[str, Any]) -> str:
    return (
        f"| {block['tp']} | {block['fp']} | {block['fn']} "
        f"| {_fmt_rate(block['precision'])} | {_fmt_rate(block['recall'])} "
        f"| {_fmt_rate(block['f1'])} | {_fmt_rate(block['false_alerts_per_day'])} "
        f"| {_fmt_rate(block['missed_event_rate'])} |"
    )


def _section(title: str, body: list[str]) -> list[str]:
    return [f"## {title}", "", *body, ""]


def render_markdown(report: dict[str, Any]) -> str:
    """Render the report as markdown; section order is fixed (REPORT_FORMAT.md)."""
    config = report["config"]
    dataset = report["dataset"]
    notes = dataset["notes"]
    lines: list[str] = [
        f"# F29 event-level evaluation report — {config['detector_id']}",
        "",
        f"> {report['disclaimer']}",
        "",
    ]

    bodies: dict[str, list[str]] = {}

    bodies["Configuration"] = [
        f"- detector_id: {config['detector_id']}",
        f"- event match tolerance: {_fmt_rate(config['event_match_tolerance_s'])} s",
        f"- split: {config['split']}",
        f"- report schema version: {report['schema_version']}",
    ]

    dataset_body = [
        "| session | duration_s | unlabeled_coverage |",
        "|---|---|---|",
    ]
    for session in dataset["session_ids"]:
        per = notes["per_session"][session]
        dataset_body.append(
            f"| {session} | {per['duration_s']:.0f} | {_fmt_rate(per['unlabeled_coverage'])} |"
        )
    dataset_body += [
        "",
        f"- total duration: {dataset['total_duration_s']:.0f} s",
        f"- unlabeled coverage (aggregate): {_fmt_rate(dataset['unlabeled_coverage'])}",
        "- data-quality notes: "
        f"raw_drops={notes['raw_drops']}, line_drops={notes['line_drops']}, "
        f"malformed_rows={notes['malformed_rows']}",
    ]
    if notes["missing_prediction_files"]:
        dataset_body.append(
            "- WARNING — sessions without a predictions file (evaluated as 0 predictions): "
            + ", ".join(notes["missing_prediction_files"])
        )
    bodies["Dataset"] = dataset_body

    metrics_body = ["### Aggregate", "", _METRIC_TABLE_HEADER, _METRIC_TABLE_SEP,
                    _metric_row(report["aggregate"])]
    for fold in report["folds"]:
        metrics_body += [
            "",
            f"### Fold {fold['fold_index'] + 1} — test session {fold['test_session']} "
            f"(detector {fold['detector_id']})",
            "",
            _METRIC_TABLE_HEADER,
            _METRIC_TABLE_SEP,
            _metric_row(fold),
        ]
    bodies["Event-level metrics"] = metrics_body

    fp_body = [
        "| session | t_start_ms | confidence | nearest_label_dt_s |",
        "|---|---|---|---|",
    ]
    for entry in report["aggregate"]["false_positives"]:
        fp_body.append(
            f"| {entry['session']} | {entry['t_start_ms']} "
            f"| {entry['confidence']:.3f} | {_fmt_rate(entry['nearest_label_dt_s'])} |"
        )
    if not report["aggregate"]["false_positives"]:
        fp_body = ["none"]
    bodies["False positives"] = fp_body

    fn_body = [
        "| session | t_start_ms | confidence | nearest_prediction_dt_s |",
        "|---|---|---|---|",
    ]
    for entry in report["aggregate"]["false_negatives"]:
        fn_body.append(
            f"| {entry['session']} | {entry['t_start_ms']} "
            f"| {entry['confidence']:.3f} | {_fmt_rate(entry['nearest_prediction_dt_s'])} |"
        )
    if not report["aggregate"]["false_negatives"]:
        fn_body = ["none"]
    bodies["False negatives"] = fn_body

    latency_body = [
        "| scope | mean_s | median_s | min_s | max_s |",
        "|---|---|---|---|---|",
        f"| aggregate | {_fmt_latency(report['aggregate']['latency'])} |",
    ]
    for fold in report["folds"]:
        latency_body.append(
            f"| {fold['test_session']} | {_fmt_latency(fold['latency'])} |"
        )
    bodies["Detection latency"] = latency_body

    coverage_body = [
        "| session | unlabeled_s | coverage |",
        "|---|---|---|",
    ]
    for session in dataset["session_ids"]:
        per = notes["per_session"][session]
        coverage_body.append(
            f"| {session} | {per['unlabeled_s']:.0f} | {_fmt_rate(per['unlabeled_coverage'])} |"
        )
    coverage_body.append(
        f"| aggregate | {notes['unlabeled_s_total']:.0f} "
        f"| {_fmt_rate(dataset['unlabeled_coverage'])} |"
    )
    bodies["Unlabeled coverage"] = coverage_body

    comparison_body = [
        "| detector_id | tp | fp | fn | precision | recall | f1 "
        "| false_alerts_per_day | missed_event_rate |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for row in report["comparison"]:
        comparison_body.append(
            f"| {row['detector_id']} | {row['tp']} | {row['fp']} | {row['fn']} "
            f"| {_fmt_rate(row['precision'])} | {_fmt_rate(row['recall'])} "
            f"| {_fmt_rate(row['f1'])} | {_fmt_rate(row['false_alerts_per_day'])} "
            f"| {_fmt_rate(row['missed_event_rate'])} |"
        )
    bodies["Model comparison"] = comparison_body

    for title in _MARKDOWN_SECTIONS:  # fixed order, regardless of dict layout
        lines += _section(title, bodies[title])
    return "\n".join(lines)
