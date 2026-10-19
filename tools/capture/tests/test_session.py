"""Session directory behavior: CSV routing, diagnostics separation, counters,
boot-change rotation, host markers, session.json (DATASET_FORMAT.md §3/§4)."""

from __future__ import annotations

import csv
import json
from pathlib import Path

import pytest

from capture import lines
from capture.session import SessionManager

GOLDEN_STREAM = [
    "#HDR,1,7,0.1.0,2,3,4,5",
    "I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0",
    "I,1020,0.0012,-0.0034,0.9988,1.5,-2.3,0.0",
    "P,1000,123456,654321",
    "F,M,1000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700",
    "F,C,1000,Walking,0.750",
    "E,1,1500,7,5000,1700000000,0.500,4100,deadbeef00010203,Good,Insufficient,"
    "UserMarked,Confirmed,Walking,Awake,5,1",
    "M,1500,user_marked#1",
    "[PIPE] window t=1000 motion=Stationary",
    "I,1000,1,2,3",  # research-shaped but malformed: counted, diverted, never repaired
]


def feed(manager: SessionManager, stream: list[str]) -> None:
    for row in stream:
        manager.feed_line(row)


def test_stream_is_split_into_files(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    feed(manager, GOLDEN_STREAM)
    directory = manager.finalize("user")

    assert (directory / "raw_imu.csv").exists()
    assert (directory / "raw_ppg.csv").exists()
    assert (directory / "features.csv").exists()
    assert (directory / "events.csv").exists()
    assert (directory / "markers.csv").exists()
    assert (directory / "diagnostics.log").exists()
    assert (directory / "session.json").exists()

    imu_csv = (directory / "raw_imu.csv").read_text(encoding="utf-8").splitlines()
    assert imu_csv[0] == "t_ms,boot_id,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps"
    assert imu_csv[1].startswith("1000,7,")
    assert len(imu_csv) == 3  # header + 2 samples

    features_csv = (directory / "features.csv").read_text(encoding="utf-8").splitlines()
    kinds = {row.split(",")[2] for row in features_csv[1:]}
    assert kinds == {"motion", "context"}
    motion_row = next(row for row in features_csv if ",motion," in row)
    assert "0.0123" in motion_row
    context_row = next(row for row in features_csv if ",context," in row)
    # Unused cells are EMPTY, never 0 (absent must stay distinguishable).
    assert ",,Unknown," not in context_row
    assert context_row.endswith(",Walking,0.75")

    events_csv = (directory / "events.csv").read_text(encoding="utf-8").splitlines()
    assert events_csv[1].startswith("1,1500,7,")
    assert "deadbeef00010203" in events_csv[1]

    markers_csv = (directory / "markers.csv").read_text(encoding="utf-8").splitlines()
    assert markers_csv[1] == "1500,7,user_marked#1,device"


def test_diagnostics_and_malformed_never_enter_the_dataset(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    feed(manager, GOLDEN_STREAM)
    directory = manager.finalize("user")

    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    assert session_json["malformed_rows"] == 1
    assert session_json["diagnostics_lines"] == 2  # the [PIPE] line + the malformed row
    assert session_json["raw_drops"] == 2 + 3 + 4  # from the #HDR counters
    assert session_json["line_drops"] == 4
    assert session_json["boot_id"] == 7
    assert session_json["firmware_version"] == "0.1.0"
    assert session_json["end_reason"] == "user"
    assert session_json["schema_version"] == lines.SCHEMA_VERSION
    assert session_json["raw_tee"] is True
    assert session_json["row_counts"]["imu"] == 2

    diagnostics = (directory / "diagnostics.log").read_text(encoding="utf-8")
    assert "[PIPE] window t=1000" in diagnostics
    assert "MALFORMED I,1000,1,2,3" in diagnostics
    for csv_name in ("raw_imu.csv", "raw_ppg.csv", "features.csv", "events.csv"):
        text = (directory / csv_name).read_text(encoding="utf-8")
        assert "[PIPE]" not in text
        assert "MALFORMED" not in text


def test_boot_id_change_rotates_the_session(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0", "I,1000,0.0,0.0,1.0,0.0,0.0,0.0"])
    feed(manager, ["#HDR,1,8,0.1.0,0,0,0,0", "I,2000,0.0,0.0,1.0,0.0,0.0,0.0"])
    manager.finalize("user")

    sessions = sorted(p for p in tmp_path.iterdir() if p.is_dir())
    assert len(sessions) == 2
    first, second = sessions
    first_json = json.loads((first / "session.json").read_text(encoding="utf-8"))
    second_json = json.loads((second / "session.json").read_text(encoding="utf-8"))
    assert first_json["end_reason"] == "boot_change"
    assert first_json["boot_id"] == 7
    assert second_json["boot_id"] == 8
    assert second_json["end_reason"] == "user"
    assert first_json["row_counts"]["imu"] == 1
    assert second_json["row_counts"]["imu"] == 1


def test_host_marker_is_recorded_with_source_host(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0"])
    manager.add_host_marker("button-press-window", t_ms=4200)
    directory = manager.finalize("eof")

    markers_csv = (directory / "markers.csv").read_text(encoding="utf-8").splitlines()
    assert markers_csv[1] == "4200,7,button-press-window,host"


def test_empty_capture_still_produces_a_session(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    directory = manager.finalize("eof")
    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    assert session_json["row_counts"]["imu"] == 0
    assert session_json["raw_tee"] is False
    assert session_json["end_reason"] == "eof"


def test_schema_mismatch_propagates_instead_of_writing(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    with pytest.raises(lines.SchemaMismatch):
        manager.feed_line("#HDR,99,0,unknown,0,0,0,0")


def test_headers_before_any_row_create_the_session(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0", "#HDR,1,7,0.1.0,0,0,0,0"])
    directory = manager.finalize("user")
    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    assert session_json["row_counts"]["header"] == 2
    assert len(list(tmp_path.iterdir())) == 1  # same boot_id: no rotation


def test_headers_log_persists_every_header_verbatim(tmp_path: Path) -> None:
    """F28 FIX: the session dir keeps the raw #HDR lines (self-describing)."""
    manager = SessionManager(tmp_path)
    feed(
        manager,
        [
            "#HDR,1,7,0.1.0,2,3,4,5",
            "I,1000,0.0,0.0,1.0,0.0,0.0,0.0",
            "#HDR,1,7,0.1.0,7,3,4,6",
        ],
    )
    directory = manager.finalize("user")

    headers = (directory / "headers.log").read_text(encoding="utf-8").splitlines()
    assert headers == ["#HDR,1,7,0.1.0,2,3,4,5", "#HDR,1,7,0.1.0,7,3,4,6"]


def test_per_source_drop_counters_and_history_survive_the_capture(tmp_path: Path) -> None:
    """F28 FIX: session.json records the per-source split AND the curve over
    time (one sample per #HDR), not only the last header's sum."""
    manager = SessionManager(tmp_path)
    feed(
        manager,
        [
            "#HDR,1,7,0.1.0,2,1,4,5",
            "I,1000,0.0,0.0,1.0,0.0,0.0,0.0",
            "#HDR,1,7,0.1.0,7,1,4,9",
        ],
    )
    directory = manager.finalize("user")
    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))

    # last #HDR wins for the point-in-time counters (existing semantics kept)
    assert session_json["imu_drops"] == 7
    assert session_json["ppg_drops"] == 1
    assert session_json["line_drops"] == 4
    assert session_json["feature_missed"] == 9
    assert session_json["raw_drops"] == 7 + 1 + 4  # unchanged sum, §3 contract

    history = session_json["drop_history"]
    assert len(history) == 2
    assert history[0]["imu_drops"] == 2 and history[0]["feature_missed"] == 5
    assert history[1]["imu_drops"] == 7 and history[1]["feature_missed"] == 9
    for sample in history:
        assert set(sample) == {"at_s", "imu_drops", "ppg_drops", "line_drops", "feature_missed"}
        assert sample["at_s"] >= 0
    assert session_json["counters_as_of_hdr_seconds"] == history[-1]["at_s"]


def test_gap_cross_check_counts_missing_imu_samples(tmp_path: Path) -> None:
    """F28 FIX: an honest host-side counter derived from the CSV itself."""
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0"])
    for t_ms in (1000, 1020, 1040, 1100, 1120, 1140):  # 60 ms gap hides 2 slots
        feed(manager, [f"I,{t_ms},0.0,0.0,1.0,0.0,0.0,0.0"])
    directory = manager.finalize("user")
    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))

    assert session_json["imu_period_ms"] == 20
    assert session_json["imu_missing_samples"] == 2
    # no PPG rows at all -> nulls, never a fabricated 0
    assert session_json["ppg_period_ms"] is None
    assert session_json["ppg_missing_samples"] is None


def test_gap_cross_check_tolerates_one_ms_jitter(tmp_path: Path) -> None:
    """19/21 ms rounding jitter must not be counted as loss."""
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0"])
    for t_ms in (1019, 1040, 1061, 1081, 1100, 1120, 1141):
        feed(manager, [f"I,{t_ms},0.0,0.0,1.0,0.0,0.0,0.0"])
    directory = manager.finalize("user")
    session_json = json.loads((directory / "session.json").read_text(encoding="utf-8"))

    assert session_json["imu_missing_samples"] == 0
    assert session_json["imu_period_ms"] in (19, 20, 21)


def test_gap_cross_check_is_null_without_enough_rows(tmp_path: Path) -> None:
    manager = SessionManager(tmp_path)
    manager.finalize("eof")
    session_json = json.loads(
        (next(tmp_path.iterdir()) / "session.json").read_text(encoding="utf-8")
    )
    assert session_json["imu_period_ms"] is None
    assert session_json["imu_missing_samples"] is None
    assert session_json["drop_history"] == []
    assert session_json["imu_drops"] == 0
    assert session_json["feature_missed"] == 0


def test_feature_booleans_write_as_0_or_1_per_dataset_contract(tmp_path: Path) -> None:
    """F28 D12: DATASET_FORMAT.md §5 says feature booleans are 0/1 (the
    F-row grammar). Python's str(bool) wrote \"True\"/\"False\" and the host
    compare treated every ppg boolean as a diff — reverting _csv_cell makes
    this test fail."""
    manager = SessionManager(tmp_path)
    feed(manager, ["#HDR,1,7,0.1.0,0,0,0,0", "F,P,1000,Good,1,63.5,0.875,0.05,100.0,0.9,0,0,0.0"])
    directory = manager.finalize("user")

    text = (directory / "features.csv").read_text(encoding="utf-8")
    assert "True" not in text and "False" not in text
    ppg = next(row for row in csv.DictReader(text.splitlines()) if row["kind"] == "ppg")
    assert ppg["hr_valid"] == "1"
    assert ppg["motion_corrupted"] == "0"
    assert ppg["hrv_valid"] == "0"
