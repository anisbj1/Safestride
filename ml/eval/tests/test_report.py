"""Report-model tests: JSON contract, markdown rendering, spec §9 "Report" rules."""

import json
from pathlib import Path
from typing import Any

import report
import run_eval
from metrics import LatencyStats, event_level_metrics
from report import build_report, comparison_row, fold_entry, metric_block, render_markdown, to_json

_CANONICAL_AGGREGATE = event_level_metrics(
    tp=2, fp=1, fn=1, matched_dt_s=[20.0, 20.0], total_duration_s=7200.0, unlabeled_s=1800.0
)


def _tiny_report(*, tp: int = 2, fp: int = 1, fn: int = 1) -> dict[str, Any]:
    """A minimal well-formed report; defaults have no FP/FN detail entries."""
    block = metric_block(
        event_level_metrics(
            tp=tp, fp=fp, fn=fn, matched_dt_s=[20.0], total_duration_s=7200.0, unlabeled_s=0.0
        )
    )
    return build_report(
        detector_id="tiny",
        tolerance_s=60.0,
        session_ids=["sA"],
        total_duration_s=7200.0,
        unlabeled_coverage=0.0,
        dataset_notes={
            "raw_drops": 0,
            "line_drops": 0,
            "malformed_rows": 0,
            "missing_prediction_files": [],
            "unlabeled_s_total": 0.0,
            "per_session": {
                "sA": {
                    "duration_s": 7200.0,
                    "unlabeled_s": 0.0,
                    "unlabeled_coverage": 0.0,
                    "raw_drops": 0,
                    "line_drops": 0,
                    "malformed_rows": 0,
                }
            },
        },
        folds=[fold_entry(fold_index=0, test_session="sA", detector_id="tiny", block=block)],
        aggregate_block=block,
        comparison=[comparison_row(detector_id="tiny", block=block)],
    )


def test_json_round_trip_is_exact(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, detector_id = canonical_sessions
    original = run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert json.loads(to_json(original)) == original


def test_two_renders_are_byte_equal(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, detector_id = canonical_sessions
    evaluation = run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert render_markdown(evaluation).encode() == render_markdown(evaluation).encode()
    assert to_json(evaluation).encode() == to_json(evaluation).encode()


def test_fp_and_fn_sections_are_separate_and_present_when_empty() -> None:
    markdown = render_markdown(_tiny_report())
    fp_at = markdown.index("## False positives")
    fn_at = markdown.index("## False negatives")
    assert fp_at < fn_at  # separate sections, fixed order (§5)

    def _section_text(start: int) -> str:
        rest = markdown[start:]
        next_header = rest.find("\n## ", 1)
        return rest[: next_header if next_header != -1 else len(rest)]

    assert _section_text(fp_at).strip().endswith("none")  # rendered even when empty
    assert _section_text(fn_at).strip().endswith("none")


def test_fp_section_lists_entries_when_present() -> None:
    block = metric_block(_CANONICAL_AGGREGATE)
    block["false_positives"] = [
        report.fp_entry(session="sA", t_start_ms=2_410_000, confidence=0.5, nearest_label_dt_s=10.0)
    ]
    evaluation = build_report(
        detector_id="tiny",
        tolerance_s=60.0,
        session_ids=["sA"],
        total_duration_s=7200.0,
        unlabeled_coverage=0.0,
        dataset_notes={
            "raw_drops": 0,
            "line_drops": 0,
            "malformed_rows": 0,
            "missing_prediction_files": [],
            "unlabeled_s_total": 0.0,
            "per_session": {
                "sA": {
                    "duration_s": 7200.0,
                    "unlabeled_s": 0.0,
                    "unlabeled_coverage": 0.0,
                    "raw_drops": 0,
                    "line_drops": 0,
                    "malformed_rows": 0,
                }
            },
        },
        folds=[],
        aggregate_block=block,
        comparison=[],
    )
    section = render_markdown(evaluation).split("## False positives")[1].split("##")[0]
    assert "2410000" in section
    assert "10.000" in section
    assert "none" not in section


def test_undefined_metrics_render_as_na() -> None:
    block = metric_block(
        event_level_metrics(
            tp=0, fp=0, fn=0, matched_dt_s=[], total_duration_s=0.0, unlabeled_s=0.0
        )
    )
    evaluation = build_report(
        detector_id="tiny",
        tolerance_s=60.0,
        session_ids=["sA"],
        total_duration_s=0.0,
        unlabeled_coverage=None,
        dataset_notes={
            "raw_drops": 0,
            "line_drops": 0,
            "malformed_rows": 0,
            "missing_prediction_files": [],
            "unlabeled_s_total": 0.0,
            "per_session": {
                "sA": {
                    "duration_s": 0.0,
                    "unlabeled_s": 0.0,
                    "unlabeled_coverage": None,
                    "raw_drops": 0,
                    "line_drops": 0,
                    "malformed_rows": 0,
                }
            },
        },
        folds=[],
        aggregate_block=block,
        comparison=[comparison_row(detector_id="tiny", block=block)],
    )
    markdown = render_markdown(evaluation)
    assert "| n/a | n/a | n/a | n/a | n/a |" in markdown
    json_text = to_json(evaluation)
    assert '"precision": null' in json_text  # D4: None → null → n/a


def test_no_accuracy_anywhere_in_the_skeleton_output(
    canonical_sessions: tuple[list[Path], str],
) -> None:
    session_dirs, detector_id = canonical_sessions
    evaluation = run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert "accuracy" not in to_json(evaluation).lower()
    assert "accuracy" not in render_markdown(evaluation).lower()  # spec §5


def test_markdown_section_order_is_fixed(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, detector_id = canonical_sessions
    markdown = render_markdown(run_eval.evaluate(session_dirs, detector_id=detector_id))
    titles = [line[3:] for line in markdown.splitlines() if line.startswith("## ")]
    assert titles == [
        "Configuration",
        "Dataset",
        "Event-level metrics",
        "False positives",
        "False negatives",
        "Detection latency",
        "Unlabeled coverage",
        "Model comparison",
    ]


def test_schema_version_is_the_spec_constant() -> None:
    assert report.K_REPORT_SCHEMA_VERSION == 1  # spec §8
    assert _tiny_report()["schema_version"] == 1


def test_comparison_has_one_row_per_detector() -> None:
    evaluation = _tiny_report()
    assert [row["detector_id"] for row in evaluation["comparison"]] == ["tiny"]
    assert evaluation["config"]["split"] == "leave-one-session-out"  # D1


def test_metric_block_latency_is_a_plain_dict() -> None:
    block = metric_block(_CANONICAL_AGGREGATE)
    assert block["latency"] == {"mean_s": 20.0, "median_s": 20.0, "min_s": 20.0, "max_s": 20.0}
    empty = metric_block(
        event_level_metrics(0, 0, 0, [], 7200.0, 0.0)
    )
    assert empty["latency"]["mean_s"] is None
    assert LatencyStats(None, None, None, None).mean_s is None
