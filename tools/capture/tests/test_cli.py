"""CLI end-to-end: a saved capture file in, a session directory out."""

from __future__ import annotations

import json
from pathlib import Path

from capture.cli import main

SAMPLE = """#HDR,1,7,0.1.0,0,0,0,0
[PIPE] window t=1000 motion=Stationary
I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0
P,1000,123456,654321
F,M,1000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700
"""


def test_cli_records_a_capture_file(tmp_path: Path) -> None:
    capture_file = tmp_path / "capture.log"
    capture_file.write_text(SAMPLE, encoding="utf-8")
    out = tmp_path / "records"

    exit_code = main(["--stdin", str(capture_file), "--out", str(out)])

    assert exit_code == 0
    sessions = list(out.iterdir())
    assert len(sessions) == 1
    session_json = json.loads((sessions[0] / "session.json").read_text(encoding="utf-8"))
    assert session_json["row_counts"] == {
        "header": 1,
        "imu": 1,
        "ppg": 1,
        "motion": 1,
        "ppg_features": 0,
        "context": 0,
        "event": 0,
        "marker": 0,
    }
    assert session_json["raw_drops"] == 0  # §15 A3 sanity check
    assert session_json["diagnostics_lines"] == 1
    assert session_json["end_reason"] == "eof"


def test_cli_writes_host_marker_into_the_session(tmp_path: Path) -> None:
    capture_file = tmp_path / "capture.log"
    capture_file.write_text(SAMPLE, encoding="utf-8")
    out = tmp_path / "records"

    exit_code = main(
        ["--stdin", str(capture_file), "--out", str(out), "--marker", "bench-run-1"]
    )

    assert exit_code == 0
    session = next(out.iterdir())
    markers_csv = (session / "markers.csv").read_text(encoding="utf-8").splitlines()
    assert markers_csv[1].endswith(",bench-run-1,host")


def test_cli_refuses_an_unknown_schema(tmp_path: Path) -> None:
    capture_file = tmp_path / "capture.log"
    capture_file.write_text("#HDR,99,0,unknown,0,0,0,0\n", encoding="utf-8")
    out = tmp_path / "records"

    exit_code = main(["--stdin", str(capture_file), "--out", str(out)])

    assert exit_code == 0  # the CLI stops cleanly and finalizes what it has
    session = next(out.iterdir())
    session_json = json.loads((session / "session.json").read_text(encoding="utf-8"))
    assert session_json["end_reason"] == "schema_change"


def test_cli_summary_reports_per_source_drops_and_warns(tmp_path: Path, capsys) -> None:
    """F28 FIX: the end-of-capture summary is loud about loss so a bench
    re-run can be judged without opening session.json."""
    capture_file = tmp_path / "capture.log"
    capture_file.write_text(
        "#HDR,1,7,0.1.0,5,0,0,2\n"
        "I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n"
        "I,1020,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n"
        "I,1040,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n"
        "I,1100,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n"
        "I,1120,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n"
        "I,1000,1,2,3\n",
        encoding="utf-8",
    )
    out = tmp_path / "records"

    exit_code = main(["--stdin", str(capture_file), "--out", str(out)])
    captured = capsys.readouterr()

    assert exit_code == 0
    assert "imu=5" in captured.out
    assert "feat_missed=2" in captured.out
    assert "2 missing samples" in captured.out
    assert "WARNING: raw_drops > 0" in captured.err
    assert "torn/malformed" in captured.err
