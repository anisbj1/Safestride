"""Python half of the 48-byte Event vector contract (DATASET_FORMAT.md §5).

The C++ twin is firmware/test/bw_research/test_bw_research/test_event_layout.cpp:
both pin the SAME bytes, so a layout drift in bw::Event fails on both sides.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import decode_events

_DATASET_FORMAT = Path(__file__).resolve().parents[3] / "docs" / "contracts" / "DATASET_FORMAT.md"
_SECTION5_MARKER = "## 5. Test vectors (golden lines, schema v1)"
_EVENT_VECTOR_MARKER = "F28-EVENT-VECTOR-48B"

# Event built field-by-field (see the C++ test for the identical construction):
# t_start_ms=123456789, seq=3, boot_id=7, duration_ms=5000,
# t_start_unix=1700000000, confidence=0.5f, reasons=4100 (bits 2 + 12),
# payload=DEADBEEF00010203, quality=(Good, Insufficient), type=UserMarked,
# label=Confirmed, context=Walking, alertness=Awake, detector_id=5,
# schema_version=1.
VECTOR_HEX = (
    "15cd5b0700000000"
    "03000000"
    "07000000"
    "88130000"
    "00f15365"
    "0000003f"
    "04100000"
    "deadbeef00010203"
    "0002030104020501"
)


def test_vector_decodes_to_the_documented_fields() -> None:
    record = decode_events.decode(bytes.fromhex(VECTOR_HEX))
    assert record["t_start_ms"] == 123456789
    assert record["seq"] == 3
    assert record["boot_id"] == 7
    assert record["duration_ms"] == 5000
    assert record["t_start_unix"] == 1700000000
    assert record["confidence"] == 0.5
    assert record["reasons"] == 4100
    assert record["reason_names"] == ["HrBelowBaseline", "SustainedLowAlertnessProbability"]
    assert record["payload_hex"] == "deadbeef00010203"
    assert record["quality_imu"] == "Good"
    assert record["quality_ppg"] == "Insufficient"
    assert record["type"] == "UserMarked"
    assert record["label"] == "Confirmed"
    assert record["activity"] == "Walking"
    assert record["alertness"] == "Awake"
    assert record["detector_id"] == 5
    assert record["schema_version"] == decode_events.EVENT_SCHEMA_VERSION


def test_vector_is_exactly_48_bytes() -> None:
    assert len(bytes.fromhex(VECTOR_HEX)) == decode_events.EVENT_SIZE


def _event_vector_from_doc() -> bytes:
    """Parse the canonical 48-byte vector out of DATASET_FORMAT.md §5.

    Parsing is pinned to exact markers (the §5 heading and the
    F28-EVENT-VECTOR-48B label line, exactly one occurrence each), so a doc
    reformat fails loudly here instead of silently dropping the drift guard
    (H6/H9). Inside the marker line, the next line is the vector; whitespace
    grouping is allowed, nothing else.
    """
    text = _DATASET_FORMAT.read_text(encoding="utf-8")
    assert text.count(_SECTION5_MARKER) == 1, "DATASET_FORMAT §5 heading moved: update this guard"
    section = text.split(_SECTION5_MARKER, 1)[1]
    assert section.count(_EVENT_VECTOR_MARKER) == 1, "event-vector marker missing or duplicated"
    after_marker = section.split(_EVENT_VECTOR_MARKER, 1)[1]
    hex_line = after_marker.splitlines()[1]  # line right after the marker line
    return bytes.fromhex("".join(hex_line.split()))


def test_vector_in_doc_matches_decoder() -> None:
    """Doc<->decoder drift guard (H6/H9, spec section 9.9): the canonical bytes
    live in DATASET_FORMAT.md section 5, so editing the doc vector OR
    decode_events alone fails here. The field expectations are hardcoded
    (CR-002/CR-011 little-endian order, reason bits), never re-derived from
    either side.
    """
    doc_vector = _event_vector_from_doc()
    assert doc_vector == bytes.fromhex(VECTOR_HEX)  # doc <-> C++/pytest twin
    record = decode_events.decode(doc_vector)
    assert record["t_start_ms"] == 123456789
    assert record["seq"] == 3
    assert record["boot_id"] == 7
    assert record["duration_ms"] == 5000
    assert record["t_start_unix"] == 1700000000
    assert record["confidence"] == 0.5
    assert record["reasons"] == 4100
    assert record["reason_names"] == ["HrBelowBaseline", "SustainedLowAlertnessProbability"]
    assert record["payload_hex"] == "deadbeef00010203"
    assert record["quality_imu"] == "Good"
    assert record["quality_ppg"] == "Insufficient"
    assert record["type"] == "UserMarked"
    assert record["label"] == "Confirmed"
    assert record["activity"] == "Walking"
    assert record["alertness"] == "Awake"
    assert record["detector_id"] == 5
    assert record["schema_version"] == decode_events.EVENT_SCHEMA_VERSION


def test_wrong_size_is_refused_not_padded() -> None:
    with pytest.raises(ValueError):
        decode_events.decode(bytes.fromhex(VECTOR_HEX)[:-1])
    with pytest.raises(ValueError):
        decode_events.decode_all(bytes.fromhex(VECTOR_HEX) + b"\x00")


def test_decode_all_reads_back_to_back_records() -> None:
    blob = bytes.fromhex(VECTOR_HEX) * 2
    records = decode_events.decode_all(blob)
    assert [r["seq"] for r in records] == [3, 3]


def test_unknown_enum_values_stay_visible() -> None:
    blob = bytearray(bytes.fromhex(VECTOR_HEX))
    blob[40] = 9  # quality.imu = 9 (not in the append-only table)
    record = decode_events.decode(bytes(blob))
    assert record["quality_imu"] == "unknown_quality_9"


def test_cli_hex_mode_emits_json(capsys: pytest.CaptureFixture[str]) -> None:
    exit_code = decode_events.main(["--hex", VECTOR_HEX])
    assert exit_code == 0
    output = capsys.readouterr().out
    assert '"type": "UserMarked"' in output


def test_cli_rejects_a_bad_blob(tmp_path) -> None:  # noqa: ANN001 - pytest fixture
    blob_file = tmp_path / "events.bin"
    blob_file.write_bytes(bytes.fromhex(VECTOR_HEX)[:-3])
    exit_code = decode_events.main([str(blob_file)])
    assert exit_code == 2
