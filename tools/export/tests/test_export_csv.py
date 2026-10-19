"""Export behavior: verbatim rows, lossy gate, refusal paths (spec §16 list 2).

Pins F28 D15/D17: data rows byte-equal to the source (timestamps and 0/1
booleans untouched, never a transform), lossy refused exit 2 / exported with
--include-lossy + counters in #META, unknown schema refused exit 4 with the
version printed, absent sources recorded in the manifest but never fabricated
as empty files, and validate-then-write (a truncated source fails before any
output file exists).
"""

from __future__ import annotations

import json
from pathlib import Path

from export import cli, csv_export, metadata
from export.session import SchemaMismatch, SessionError, load_session

FIXTURE_CSVS = ("raw_imu.csv", "raw_ppg.csv", "features.csv", "events.csv", "markers.csv")


def _data_lines(exported: Path) -> list[str]:
    return [
        line
        for line in exported.read_text(encoding="utf-8").splitlines()
        if not line.startswith("#")
    ]


def _set_meta(session_dir: Path, **updates: object) -> None:
    path = session_dir / "session.json"
    meta = json.loads(path.read_text(encoding="utf-8"))
    meta.update(updates)
    path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")


def test_data_rows_are_byte_equal_to_source(fixture_session, out_dir) -> None:
    session = load_session(fixture_session)
    result = csv_export.write_session_export(session, out_dir)
    for file_name in result.exported:
        source = (fixture_session / file_name).read_text(encoding="utf-8").splitlines()
        exported = _data_lines(result.out_dir / file_name)
        assert exported == source, f"{file_name} rows transformed by the export"
        # Spot-check the contract details verbatim-copy must preserve:
        # timestamps as written, booleans as 0/1 (never True/False).
        for line in exported[1:]:
            assert "True" not in line and "False" not in line


def test_unknown_schema_version_is_refused_with_the_version_printed(fixture_session) -> None:
    _set_meta(fixture_session, schema_version=2)
    try:
        load_session(fixture_session)
    except SchemaMismatch as exc:
        assert "2" in str(exc)
    else:
        raise AssertionError("schema_version=2 must be refused")


def test_lossy_session_is_refused_by_default(fixture_session, out_dir, capsys) -> None:
    _set_meta(fixture_session, raw_drops=5, imu_drops=5)
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_REFUSED_LOSSY
    assert "raw_drops=5" in capsys.readouterr().err
    assert not any(out_dir.iterdir())  # nothing written before the gate


def test_include_lossy_exports_with_counters_in_meta(fixture_session, out_dir) -> None:
    _set_meta(fixture_session, raw_drops=5, imu_drops=5, malformed_rows=2)
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir), "--include-lossy"])
    assert exit_code == cli.EXIT_OK
    exported = (out_dir / "research_20261017T000000Z" / "raw_imu.csv").read_text(encoding="utf-8")
    assert f"{metadata.META_ROW_PREFIX},session,raw_drops,5" in exported
    assert f"{metadata.META_ROW_PREFIX},session,malformed_rows,2" in exported


def test_absent_source_is_recorded_never_fabricated(fixture_session, out_dir) -> None:
    (fixture_session / "raw_ppg.csv").unlink()
    _set_meta(fixture_session, raw_tee=False)
    session = load_session(fixture_session)
    result = csv_export.write_session_export(session, out_dir)
    assert "raw_ppg.csv" not in result.exported
    assert "raw_ppg.csv" in result.absent
    assert not (result.out_dir / "raw_ppg.csv").exists()  # never fabricated empty
    manifest = json.loads((result.out_dir / "export_manifest.json").read_text(encoding="utf-8"))
    assert "raw_ppg.csv" in manifest["absent_files"]
    assert "raw_ppg.csv" not in manifest["exported_files"]


def test_marker_source_column_is_preserved(fixture_session, out_dir) -> None:
    result = csv_export.write_session_export(load_session(fixture_session), out_dir)
    rows = _data_lines(result.out_dir / "markers.csv")[1:]
    assert any(row.endswith(",device") for row in rows)
    assert any(row.endswith(",host") for row in rows)


def test_labels_csv_is_exported_iff_present(fixture_session, out_dir) -> None:
    session = load_session(fixture_session)
    assert "labels" not in session.tables
    result = csv_export.write_session_export(session, out_dir)
    assert "labels.csv" not in result.exported

    (fixture_session / "labels.csv").write_text(
        "t_ms,boot_id,label\n5000,7,Confirmed\n", encoding="utf-8"
    )
    result = csv_export.write_session_export(load_session(fixture_session), out_dir)
    assert "labels.csv" in result.exported
    lines = _data_lines(result.out_dir / "labels.csv")
    assert lines[0] == "t_ms,boot_id,label"
    assert lines[1] == "5000,7,Confirmed"


def test_truncated_source_csv_fails_before_any_output(fixture_session, out_dir) -> None:
    imu = fixture_session / "raw_imu.csv"
    lines = imu.read_text(encoding="utf-8").splitlines()
    imu.write_text("\n".join(lines[:2] + [lines[2].rsplit(",", 1)[0]]) + "\n", encoding="utf-8")
    try:
        load_session(fixture_session)
    except SessionError as exc:
        assert "truncated" in str(exc)
    else:
        raise AssertionError("a truncated source row must fail validation")
    # CLI-level: validate-then-write means the out dir stays empty.
    exit_code = cli.main([str(fixture_session), "--out", str(out_dir)])
    assert exit_code == cli.EXIT_BAD_SESSION
    assert not any(out_dir.iterdir())
