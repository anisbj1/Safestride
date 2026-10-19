"""CLI surface: exit-code map, --include-lossy, target protection (§16 list 4)."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from export import cli

FIXTURE_NAME = "research_20261017T000000Z"


def _set_meta(session_dir: Path, **updates: object) -> None:
    path = session_dir / "session.json"
    meta = json.loads(path.read_text(encoding="utf-8"))
    meta.update(updates)
    path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")


def test_happy_path_exports_every_source(fixture_session, out_dir, capsys) -> None:
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_OK
    target = out_dir / FIXTURE_NAME
    for file_name in ("raw_imu.csv", "raw_ppg.csv", "features.csv", "events.csv", "markers.csv"):
        assert (target / file_name).exists()
    assert (target / "export_manifest.json").exists()
    out = capsys.readouterr().out
    assert FIXTURE_NAME in out
    assert "events: 1 rows" in out


def test_include_lossy_flag_lifts_the_gate(fixture_session, out_dir) -> None:
    _set_meta(fixture_session, raw_drops=3, ppg_drops=3)
    exit_code = cli.main(
        [str(fixture_session), "--out", str(out_dir), "--include-lossy"]
    )
    assert exit_code == cli.EXIT_OK
    assert (out_dir / FIXTURE_NAME / "raw_imu.csv").exists()


def test_lossy_default_exit_code_is_two(fixture_session, out_dir, capsys) -> None:
    _set_meta(fixture_session, raw_drops=3, ppg_drops=3)
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_REFUSED_LOSSY
    assert "--include-lossy" in capsys.readouterr().err


def test_malformed_rows_alone_are_lossy(fixture_session, out_dir) -> None:
    _set_meta(fixture_session, malformed_rows=1)
    assert cli.main([str(fixture_session), "--out", str(out_dir)]) == cli.EXIT_REFUSED_LOSSY


def test_unknown_schema_exit_code_is_four(fixture_session, out_dir, capsys) -> None:
    _set_meta(fixture_session, schema_version=99)
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_BAD_SESSION
    assert "99" in capsys.readouterr().err


def test_missing_session_json_exits_four(tmp_path, out_dir, capsys) -> None:
    empty = tmp_path / "not_a_session"
    empty.mkdir()
    exit_code = cli.main([str(empty), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_BAD_SESSION
    assert "session.json" in capsys.readouterr().err


def test_non_empty_out_target_is_never_overwritten(fixture_session, out_dir, capsys) -> None:
    sentinel = out_dir / "precious.csv"
    sentinel.write_text("do not touch\n", encoding="utf-8")
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_BAD_SESSION
    assert sentinel.read_text(encoding="utf-8") == "do not touch\n"
    assert "not empty" in capsys.readouterr().err


def test_multiple_sessions_export_into_one_out(fixture_session, out_dir) -> None:
    second = fixture_session.parent / "session_second"
    shutil.copytree(fixture_session, second)
    _set_meta(second, session_id="research_20261017T000001Z")
    exit_code = cli.main(
        [str(fixture_session), str(second), "--out", str(out_dir)]
    )
    assert exit_code == cli.EXIT_OK
    assert (out_dir / FIXTURE_NAME / "raw_imu.csv").exists()
    assert (out_dir / "research_20261017T000001Z" / "raw_imu.csv").exists()


def test_help_is_clean(capsys) -> None:
    with pytest.raises(SystemExit) as excinfo:
        cli.main(["--help"])
    assert excinfo.value.code == 0
    assert "include-lossy" in capsys.readouterr().out


def test_exit_code_constants_match_the_compare_map() -> None:
    # Same exit-code map as tools/replay/compare.py (spec §16): 2 = lossy
    # recording, 4 = missing session/unknown schema; 3 stays compare-only.
    assert (cli.EXIT_OK, cli.EXIT_REFUSED_LOSSY, cli.EXIT_BAD_SESSION) == (0, 2, 4)
