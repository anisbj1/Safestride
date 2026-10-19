"""F28 replay round-trip (spec §9.6): replaying the checked-in fixture must
reproduce its recorded features.csv (exit 0); a lossy session is refused
(exit 2, never a pass); a real value drift is detected (exit 3); missing
inputs are usage errors (exit 4).

The replay binary is built once per test session (`pio run -d tools/replay`)
and skipped when PlatformIO is unavailable.
"""

from __future__ import annotations

import csv
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPLAY_DIR = Path(__file__).resolve().parents[1]
TOOLS_DIR = REPLAY_DIR.parent
COMPARE = REPLAY_DIR / "compare.py"
FIXTURE = REPLAY_DIR / "fixtures" / "session_basic"


@pytest.fixture(scope="session")
def replay_binary() -> Path:
    pio = shutil.which("pio")
    if pio is None:
        candidate = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"
        if not candidate.exists():
            pytest.skip("pio not found — replay binary cannot be built here")
        pio = str(candidate)
    result = subprocess.run(
        [pio, "run", "-d", str(REPLAY_DIR)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    for name in ("program.exe", "program"):
        binary = REPLAY_DIR / ".pio" / "build" / "native" / name
        if binary.exists():
            return binary
    pytest.fail("replay build succeeded but the binary is missing")


def run_compare(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(COMPARE), *args], capture_output=True, text=True, check=False
    )


def patched_fixture(tmp_path: Path, mutate) -> Path:  # noqa: ANN001 - small helper
    session = tmp_path / "session_basic"
    shutil.copytree(FIXTURE, session)
    meta_path = session / "session.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    mutate(meta)
    meta_path.write_text(json.dumps(meta), encoding="utf-8")
    return session


def test_round_trip_reproduces_the_fixture(replay_binary: Path) -> None:
    result = run_compare(str(FIXTURE), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "match:" in result.stdout


def test_raw_drops_invalidate_the_comparison(replay_binary: Path, tmp_path: Path) -> None:
    session = patched_fixture(tmp_path, lambda meta: meta.update(raw_drops=3))
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 2  # never a pass, never a mismatch: invalid
    assert "raw_drops" in result.stderr


def test_malformed_rows_invalidate_the_comparison(replay_binary: Path, tmp_path: Path) -> None:
    session = patched_fixture(tmp_path, lambda meta: meta.update(malformed_rows=1))
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 2
    assert "malformed_rows" in result.stderr


def test_value_drift_beyond_tolerance_is_a_mismatch(replay_binary: Path, tmp_path: Path) -> None:
    session = tmp_path / "session_basic"
    shutil.copytree(FIXTURE, session)
    features = session / "features.csv"
    text = features.read_text(encoding="utf-8")
    # Perturb one recorded float well beyond rel=1e-3 / abs=1e-4.
    features.write_text(text.replace(",0.0006,", ",0.0060,", 1), encoding="utf-8")
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 3
    assert "MISMATCH" in result.stderr


def test_missing_binary_is_a_usage_error(tmp_path: Path) -> None:
    result = run_compare(str(FIXTURE), "--binary", str(tmp_path / "nope.exe"))
    assert result.returncode == 4
    assert "binary not found" in result.stderr


def test_missing_session_is_a_usage_error(tmp_path: Path) -> None:
    result = run_compare(str(tmp_path / "empty"))
    assert result.returncode == 4
    assert "session.json" in result.stderr


# -- D12: recording-precision envelope, input coverage, lost-row attribution


def copied_fixture(tmp_path: Path) -> Path:
    session = tmp_path / "session_basic"
    shutil.copytree(FIXTURE, session)
    return session


def drop_imu_rows(session: Path, drop_times: set[int]) -> None:
    path = session / "raw_imu.csv"
    lines = path.read_text(encoding="utf-8").splitlines()
    kept = [row for row in lines[1:] if int(row.split(",")[0]) not in drop_times]
    path.write_text("\n".join([lines[0], *kept]) + "\n", encoding="utf-8")


def write_features_from_replay(replay_binary: Path, session: Path) -> None:
    """Rebuild features.csv from what the replay reproduces from the
    session's OWN raw rows — every row matches by construction, so a test
    can then inject exactly the one row-level difference it wants to probe."""
    sys.path.insert(0, str(TOOLS_DIR))
    from capture import lines as linemod
    from capture.session import FEATURES_COLUMNS, _csv_cell

    result = subprocess.run(
        [str(replay_binary), str(session)], capture_output=True, text=True, check=True
    )
    entries = []
    for line in result.stdout.splitlines():
        if not linemod.is_research_line(line):
            continue
        kind, row = linemod.parse_line(line)
        csv_kind = {"motion": "motion", "ppg_features": "ppg"}.get(kind)
        if csv_kind is None:
            continue
        entry = {"t_ms": row["t_ms"], "boot_id": 0, "kind": csv_kind}
        for column in FEATURES_COLUMNS:
            if column not in entry:
                value = row.get(column)
                entry[column] = _csv_cell("" if value is None else value)
        entries.append(entry)
    with (session / "features.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FEATURES_COLUMNS)
        writer.writeheader()
        writer.writerows(entries)


def edit_feature_row(session: Path, t_ms: str, kind: str, **updates: str) -> None:
    features = session / "features.csv"
    with features.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        columns = reader.fieldnames
        rows = [dict(row) for row in reader]
    row = next(r for r in rows if r["t_ms"] == t_ms and r["kind"] == kind)
    row.update(updates)
    with features.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def remove_feature_row(session: Path, t_ms: str, kind: str) -> None:
    features = session / "features.csv"
    with features.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        columns = reader.fieldnames
        rows = [dict(row) for row in reader]
    with features.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(r for r in rows if not (r["t_ms"] == t_ms and r["kind"] == kind))


def bump_feature_row(
    session: Path, t_ms: str, kind: str, field: str, delta: float
) -> None:
    features = session / "features.csv"
    with features.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        columns = reader.fieldnames
        rows = [dict(row) for row in reader]
    row = next(r for r in rows if r["t_ms"] == t_ms and r["kind"] == kind)
    row[field] = f"{float(row[field]) + delta:.6f}"
    with features.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def test_legacy_bool_words_compare_by_value(replay_binary: Path, tmp_path: Path) -> None:
    """DATASET_FORMAT §5 stores booleans as 0/1, but sessions captured
    before 2026-10-13 wrote str(bool) (\"False\"). Compare must match them by
    value — reverting the word mapping turns this into exit 3 (D12)."""
    session = copied_fixture(tmp_path)
    edit_feature_row(session, "1000", "ppg", hr_valid="False")  # legacy spelling
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "match:" in result.stdout


def test_window_before_the_recording_is_not_comparable(
    replay_binary: Path, tmp_path: Path
) -> None:
    """Capture attached mid-stream: the first feature window's inputs were
    never recorded, so the replay cannot recompute it — the DEVICE row for
    it (computed from pre-attach samples) must be reported and excluded
    (exit 0), never diffed. Without the coverage check the mismatching first
    row turns this into exit 3 (D12)."""
    session = copied_fixture(tmp_path)
    drop_imu_rows(session, set(range(0, 1200, 20)))  # recording starts at 1200
    write_features_from_replay(replay_binary, session)
    # The device emitted this window from samples the host never saw:
    edit_feature_row(
        session, "1000", "motion", quality="Good", enmo_g="0.0328",
        jerk_rms_g_per_s="0.2969", gyro_rms_dps="4.1", gravity_tilt_deg="5.0",
    )
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "not comparable" in result.stdout
    assert "window starts before the recording" in result.stdout


def test_window_overlapping_sample_loss_is_not_comparable(
    replay_binary: Path, tmp_path: Path
) -> None:
    """UART tearing removed samples inside one window: those inputs are not
    in the recording, so that window is unverifiable — the device's value
    (computed from the full inputs) must be reported and excluded, never
    diffed. Without the coverage check compare diffs it and exits 3 (D12)."""
    session = copied_fixture(tmp_path)
    drop_imu_rows(session, {6000, 6020})  # a 60 ms hole: 2 missing slots
    write_features_from_replay(replay_binary, session)
    # Device jerk for that window came from the complete inputs; the replay
    # lost 2 of 50 samples, so its value drifted far beyond the envelope:
    bump_feature_row(session, "7000", "motion", "jerk_rms_g_per_s", 0.05)
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "overlaps recorded sample loss" in result.stdout
    assert "motion t_ms=7000" in result.stdout


def test_drift_within_print_bin_reach_is_consistent(
    replay_binary: Path, tmp_path: Path
) -> None:
    """The device computes features from full-precision samples; the replay
    recomputes from %.4f-rounded inputs, so values within the measured
    print-bin envelope are CONSISTENT, not mismatches (D12). Reverting the
    envelope check turns this into exit 3; it must still exceed base
    tolerance for the check to exercise the envelope at all."""
    session = copied_fixture(tmp_path)
    features = session / "features.csv"
    with features.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        columns = reader.fieldnames
        rows = [dict(row) for row in reader]
    motion = next(row for row in rows if row["kind"] == "motion")
    original = float(motion["enmo_g"])
    motion["enmo_g"] = drifted = f"{original + 0.0002:.4f}"  # 2e-4 > abs tol
    # 1e-4 (so base tolerance alone rejects it) but <= 1e-4 + 2e-4 envelope.
    assert float(drifted) - original > 1e-4
    with features.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "consistent within recording precision" in result.stdout


def test_extra_replay_row_without_evidence_is_a_mismatch(
    replay_binary: Path, tmp_path: Path
) -> None:
    """A replay row the recording lacks means the device row never arrived:
    with no torn-line evidence that is a real loss → exit 3 (never a silent
    pass)."""
    session = copied_fixture(tmp_path)
    remove_feature_row(session, "6000", "motion")
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 3
    assert "extra motion row at t_ms=6000" in result.stderr


def test_extra_replay_row_with_torn_evidence_is_reported_lost(
    replay_binary: Path, tmp_path: Path
) -> None:
    """Torn research fragments in diagnostics.log prove the recording lost
    rows the device emitted — then the extra replay row is reported as lost
    in recording and the comparison still passes (D12)."""
    session = copied_fixture(tmp_path)
    remove_feature_row(session, "6000", "motion")
    (session / "diagnostics.log").write_text(
        "﻿��0,Insufficient,0,0.0,0.000,0.00000,0.0,0.000,0,0,0.0\n"
        "��I,105656,0.0852,-0.0083,1.0259,-4.0,0.7,0.0\n",
        encoding="utf-8",
    )
    result = run_compare(str(session), "--binary", str(replay_binary))
    assert result.returncode == 0, f"{result.stdout}\n{result.stderr}"
    assert "lost in recording" in result.stdout
    assert "torn research fragments" in result.stdout
