"""End-to-end tests on synthetic F28 sessions (F29 spec §9 "End-to-end", §14 DoD).

Every expected value below is hand-computed from the canonical two-session
set (spec §9): sA 7200 s with labels [1200, 3600, 5800] (one ``Rejected`` at
2400, 600 s ``Unlabeled``), sB 5400 s with labels [1000, 4000]; the stub
fires at [1220, 3580, 5850, 2410] and [1030, 4050].
"""

import json
import time
from pathlib import Path

import pytest
import report as report_mod
import run_eval
import synthetic
from event_matching import match_events
from run_eval import load_predictions, load_session

_REPO_ROOT = Path(__file__).resolve().parents[3]
_EXAMPLE_DIR = _REPO_ROOT / "docs" / "evaluation" / "example"


def _read_example(name: str) -> str:
    # Universal newlines: the drift guard compares content, not line-ending
    # style, so CRLF checkouts (repo backlog item 9) pass unchanged.
    return (_EXAMPLE_DIR / name).read_text(encoding="utf-8", newline=None)


def test_fixture_dirs_parse_as_f28_sessions(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, _detector_id = canonical_sessions
    session_a = load_session(session_dirs[0])
    session_b = load_session(session_dirs[1])
    assert session_a.session_id == "synthetic_sA"
    assert session_a.duration_s == 7200.0
    assert [e.t_start_ms // 1000 for e in session_a.confirmed_labels] == [1200, 3600, 5800]
    assert [e.t_start_ms // 1000 for e in session_a.rejected_labels] == [2400]
    assert session_a.unlabeled_s == 600.0
    assert (session_a.raw_drops, session_a.line_drops, session_a.malformed_rows) == (0, 0, 0)
    assert session_b.session_id == "synthetic_sB"
    assert session_b.duration_s == 5400.0
    assert len(session_b.confirmed_labels) == 2
    assert session_b.unlabeled_s == 0.0


def test_stub_fires_at_the_expected_times(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, _detector_id = canonical_sessions
    predictions_a = load_predictions(session_dirs[0] / "predictions_stub_stillness.csv")
    predictions_b = load_predictions(session_dirs[1] / "predictions_stub_stillness.csv")
    assert [p.t_start_ms // 1000 for p in predictions_a] == [1220, 2410, 3580, 5850]
    assert [p.t_start_ms // 1000 for p in predictions_b] == [1030, 4050]
    assert all(p.confidence == synthetic.K_STUB_CONFIDENCE for p in predictions_a + predictions_b)


def test_per_session_counts_match_the_hand_computation(
    canonical_sessions: tuple[list[Path], str],
) -> None:
    session_dirs, _detector_id = canonical_sessions
    session_a = load_session(session_dirs[0])
    session_b = load_session(session_dirs[1])
    result_a = match_events(
        session_a.confirmed_labels,
        load_predictions(session_dirs[0] / "predictions_stub_stillness.csv"),
    )
    result_b = match_events(
        session_b.confirmed_labels,
        load_predictions(session_dirs[1] / "predictions_stub_stillness.csv"),
    )
    assert (result_a.tp, result_a.fp, result_a.fn) == (3, 1, 0)
    assert result_a.matched_dt_s == pytest.approx((20.0, -20.0, 50.0))
    assert (result_b.tp, result_b.fp, result_b.fn) == (2, 0, 0)
    assert result_b.matched_dt_s == pytest.approx((30.0, 50.0))


def test_aggregate_counts_rates_and_latency(
    canonical_sessions: tuple[list[Path], str],
) -> None:
    session_dirs, detector_id = canonical_sessions
    aggregate = run_eval.evaluate(session_dirs, detector_id=detector_id)["aggregate"]
    assert (aggregate["tp"], aggregate["fp"], aggregate["fn"]) == (5, 1, 0)
    assert aggregate["precision"] == pytest.approx(5 / 6)
    assert aggregate["recall"] == pytest.approx(1.0)
    assert aggregate["f1"] == pytest.approx(10 / 11)
    assert aggregate["false_alerts_per_day"] == pytest.approx(86400 / 12600)  # ≈ 6.857
    assert aggregate["missed_event_rate"] == pytest.approx(0.0)
    assert aggregate["latency"]["mean_s"] == pytest.approx(26.0)
    assert aggregate["latency"]["median_s"] == pytest.approx(30.0)
    assert aggregate["latency"]["min_s"] == pytest.approx(-20.0)
    assert aggregate["latency"]["max_s"] == pytest.approx(50.0)


def test_unlabeled_coverage_aggregate_and_per_session(
    canonical_sessions: tuple[list[Path], str],
) -> None:
    session_dirs, detector_id = canonical_sessions
    evaluation = run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert evaluation["dataset"]["unlabeled_coverage"] == pytest.approx(600 / 12600)  # ≈ 0.0476
    per_session = evaluation["dataset"]["notes"]["per_session"]
    assert per_session["synthetic_sA"]["unlabeled_coverage"] == pytest.approx(600 / 7200)
    assert per_session["synthetic_sB"]["unlabeled_coverage"] == pytest.approx(0.0)
    assert evaluation["dataset"]["total_duration_s"] == pytest.approx(12600.0)


def test_false_positive_and_negative_details(
    canonical_sessions: tuple[list[Path], str],
) -> None:
    session_dirs, detector_id = canonical_sessions
    aggregate = run_eval.evaluate(session_dirs, detector_id=detector_id)["aggregate"]
    # The lone FP is the stub firing at 2410 s, 10 s after the Rejected label.
    assert aggregate["false_positives"] == [
        {
            "session": "synthetic_sA",
            "t_start_ms": 2_410_000,
            "confidence": 0.5,
            "nearest_label_dt_s": 10.0,
        }
    ]
    assert aggregate["false_negatives"] == []  # no missed label anywhere


def test_leave_one_session_out_structure(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, detector_id = canonical_sessions
    evaluation = run_eval.evaluate(session_dirs, detector_id=detector_id)
    folds = evaluation["folds"]
    assert len(folds) == 2  # one fold per session (D1)
    assert [fold["test_session"] for fold in folds] == ["synthetic_sA", "synthetic_sB"]
    assert (folds[0]["tp"], folds[0]["fp"], folds[0]["fn"]) == (3, 1, 0)
    assert (folds[1]["tp"], folds[1]["fp"], folds[1]["fn"]) == (2, 0, 0)
    assert evaluation["config"]["split"] == "leave-one-session-out"


def test_harness_runs_under_two_seconds(canonical_sessions: tuple[list[Path], str]) -> None:
    session_dirs, detector_id = canonical_sessions
    started = time.perf_counter()
    run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert time.perf_counter() - started < 2.0  # spec §11 budget


def test_cli_exit_ok_and_writes_both_reports(
    canonical_sessions: tuple[list[Path], str], tmp_path: Path
) -> None:
    session_dirs, detector_id = canonical_sessions
    out_dir = tmp_path / "out"
    code = run_eval.main(
        ["--sessions", *(str(d) for d in session_dirs), "--out", str(out_dir)]
    )
    assert code == run_eval.K_EXIT_OK == 0
    assert (out_dir / "report.json").exists()
    assert (out_dir / "report.md").exists()


def test_cli_exit_input_error_on_missing_session_json(tmp_path: Path) -> None:
    empty_session = tmp_path / "research_empty"
    empty_session.mkdir()
    out_dir = tmp_path / "out"
    code = run_eval.main(["--sessions", str(empty_session), "--out", str(out_dir)])
    assert code == run_eval.K_EXIT_INPUT_ERROR == 2
    assert not (out_dir / "report.json").exists()


def test_cli_exit_schema_version_on_unknown_schema(tmp_path: Path) -> None:
    drifted = tmp_path / "research_drifted"
    drifted.mkdir()
    (drifted / "session.json").write_text(
        json.dumps({"schema_version": 99, "session_id": "research_drifted"}),
        encoding="utf-8",
        newline="",
    )
    code = run_eval.main(["--sessions", str(drifted), "--out", str(tmp_path / "out")])
    assert code == run_eval.K_EXIT_SCHEMA_VERSION == 4


def test_cli_exit_input_error_on_single_session(tmp_path: Path) -> None:
    # One session cannot form a leave-one-session-out fold — a clean input
    # error (exit 2), never an unhandled ValueError traceback.
    session_dir = synthetic.write_session(
        tmp_path / "sessions",
        session_id="synthetic_single",
        started_utc="2026-01-01T00:00:00Z",
        duration_s=1200,
        confirmed_labels_s=(100,),
        rejected_labels_s=(),
        unlabeled_windows_s=(),
        bursts_s=(100,),
    )
    synthetic.run_stub_detector(session_dir)
    out_dir = tmp_path / "out"
    code = run_eval.main(["--sessions", str(session_dir), "--out", str(out_dir)])
    assert code == run_eval.K_EXIT_INPUT_ERROR == 2
    assert not (out_dir / "report.json").exists()


def test_cli_exit_input_error_on_duplicate_session_ids(tmp_path: Path) -> None:
    # Two directories claiming the same session_id are ambiguous input — exit 2.
    session_dirs = []
    for root_name in ("first", "second"):
        session_dir = synthetic.write_session(
            tmp_path / root_name,
            session_id="synthetic_dup",
            started_utc="2026-01-01T00:00:00Z",
            duration_s=1200,
            confirmed_labels_s=(100,),
            rejected_labels_s=(),
            unlabeled_windows_s=(),
            bursts_s=(100,),
        )
        synthetic.run_stub_detector(session_dir)
        session_dirs.append(session_dir)
    out_dir = tmp_path / "out"
    code = run_eval.main(
        ["--sessions", *(str(d) for d in session_dirs), "--out", str(out_dir)]
    )
    assert code == run_eval.K_EXIT_INPUT_ERROR == 2
    assert not (out_dir / "report.json").exists()


def test_cli_rejects_non_positive_tolerance(
    canonical_sessions: tuple[list[Path], str], tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    # A tolerance of 0 or less is nonsense (dt ≤ tolerance never/always holds);
    # argparse usage errors exit 2, matching K_EXIT_INPUT_ERROR (REPORT_FORMAT §6).
    session_dirs, _detector_id = canonical_sessions
    for bad in ("0", "-1.5"):
        with pytest.raises(SystemExit) as excinfo:
            run_eval.main(
                [
                    "--sessions",
                    *(str(d) for d in session_dirs),
                    "--out",
                    str(tmp_path / "out"),
                    "--tolerance-s",
                    bad,
                ]
            )
        assert excinfo.value.code == run_eval.K_EXIT_INPUT_ERROR == 2
    assert "tolerance must be > 0" in capsys.readouterr().err


def test_cli_rejects_infinite_tolerance(
    canonical_sessions: tuple[list[Path], str], tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    # An infinite tolerance would match every prediction to every label,
    # silently inflating TP/recall — nonsense input, exit 2 like <= 0
    # (REPORT_FORMAT §6; 00 §7.5: never fabricate confidence from bad data).
    # Reverting the isfinite guard in _tolerance_s_arg makes this fail:
    # float("inf") > 0 is True, so argparse would accept it and main()
    # would return 0 instead of raising SystemExit.
    session_dirs, _detector_id = canonical_sessions
    for bad in ("inf", "-inf"):
        with pytest.raises(SystemExit) as excinfo:
            run_eval.main(
                [
                    "--sessions",
                    *(str(d) for d in session_dirs),
                    "--out",
                    str(tmp_path / "out"),
                    "--tolerance-s",
                    bad,
                ]
            )
        assert excinfo.value.code == run_eval.K_EXIT_INPUT_ERROR == 2
    assert "tolerance must be finite" in capsys.readouterr().err


def test_missing_predictions_file_warns_and_is_not_an_error(tmp_path: Path) -> None:
    # Documented guarantee (REPORT_FORMAT §4/§5): a session without its
    # predictions file is evaluated as 0 predictions + a WARNING note —
    # never silent, never a hard error.
    session_dirs, detector_id = synthetic.write_canonical_sessions(tmp_path / "sessions")
    (session_dirs[1] / f"predictions_{detector_id}.csv").unlink()  # sB loses its predictions
    out_dir = tmp_path / "out"
    code = run_eval.main(
        ["--sessions", *(str(d) for d in session_dirs), "--out", str(out_dir)]
    )
    assert code == run_eval.K_EXIT_OK == 0  # not a hard error
    report = json.loads((out_dir / "report.json").read_text(encoding="utf-8"))
    assert report["dataset"]["notes"]["missing_prediction_files"] == [
        f"synthetic_sB:{detector_id}"
    ]
    # sB had 2 confirmed labels and now 0 predictions → both are false negatives.
    fold_b = next(f for f in report["folds"] if f["test_session"] == "synthetic_sB")
    assert (fold_b["tp"], fold_b["fp"], fold_b["fn"]) == (0, 0, 2)
    markdown = (out_dir / "report.md").read_text(encoding="utf-8")
    assert "WARNING — sessions without a predictions file" in markdown
    assert f"synthetic_sB:{detector_id}" in markdown


def test_example_report_matches_committed_files(tmp_path: Path) -> None:
    """Drift guard (D3): regenerate → byte-compare against the committed example.

    Everything that produces the example — fixture writer, stub detector,
    matching, metrics, report shaping — is exercised here, so mutating any of
    them without regenerating ``docs/evaluation/example/`` fails this test.
    Cited by name in REPORT_FORMAT.md §7.
    """
    session_dirs, detector_id = synthetic.write_canonical_sessions(tmp_path / "regen")
    evaluation = run_eval.evaluate(session_dirs, detector_id=detector_id)
    assert report_mod.to_json(evaluation) == _read_example("report.json")
    assert report_mod.render_markdown(evaluation) == _read_example("report.md")
