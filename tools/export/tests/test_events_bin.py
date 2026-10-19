"""Binary event path: doc vector <-> decode_events <-> export adapter (D18).

The chain is pinned end to end: the ``F28-EVENT-VECTOR-48B`` bytes are read
from ``DATASET_FORMAT.md`` §5, decoded through ``tools/decode_events.py``
(sole owner of the 48-byte layout), and normalized by
``events_source.event_csv_row`` to the same cell shape the §5 golden
``E,...`` line spells out. Editing the doc vector, the decoder or the
export adapter alone fails this suite.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import decode_events
from export import events_source
from export.events_source import EVENT_RECORD_BYTES, read_events_bin, read_events_csv

_DATASET_FORMAT = Path(__file__).resolve().parents[3] / "docs" / "contracts" / "DATASET_FORMAT.md"
_SECTION5_MARKER = "## 5. Test vectors (golden lines, schema v1)"
_EVENT_VECTOR_MARKER = "F28-EVENT-VECTOR-48B"

# The §5 golden E line, mirrored here field-for-field (the doc parse below
# is asserted equal to this literal, so a doc edit is caught immediately).
GOLDEN_E_LINE = (
    "E,3,123456789,7,5000,1700000000,0.500,4100,deadbeef00010203,"
    "Good,Insufficient,UserMarked,Confirmed,Walking,Awake,5,1"
)


def _doc_section5() -> str:
    text = _DATASET_FORMAT.read_text(encoding="utf-8")
    assert text.count(_SECTION5_MARKER) == 1
    return text.split(_SECTION5_MARKER, 1)[1].split("## 6.", 1)[0]


def _doc_vector() -> bytes:
    section = _doc_section5()
    assert section.count(_EVENT_VECTOR_MARKER) == 1
    after = section.split(_EVENT_VECTOR_MARKER, 1)[1]
    return bytes.fromhex("".join(after.splitlines()[1].split()))


def test_event_record_bytes_is_imported_not_redefined() -> None:
    # [DERIVED] constant from the spec §16 table, asserted against the
    # decoder: the layout itself stays owned by decode_events.py alone.
    assert EVENT_RECORD_BYTES == decode_events.EVENT_SIZE


def test_binary_vector_decodes_to_the_doc_golden_e_line(tmp_path) -> None:
    section = _doc_section5()
    golden = [line for line in section.splitlines() if line.startswith("E,")]
    assert golden == [GOLDEN_E_LINE], "§5 golden E line drifted: update this mirror"

    blob = tmp_path / "events.bin"
    blob.write_bytes(_doc_vector())
    records = read_events_bin(blob)
    assert len(records) == 1

    cells = events_source.event_csv_row(records[0])
    expected = dict(
        zip(
            [
                "seq",
                "t_start_ms",
                "boot_id",
                "duration_ms",
                "t_start_unix",
                "confidence",
                "reasons",
                "payload_hex",
                "quality_imu",
                "quality_ppg",
                "type",
                "label",
                "activity",
                "alertness",
                "detector_id",
                "schema_version",
            ],
            GOLDEN_E_LINE.split(",")[1:],
            strict=True,
        )
    )
    assert cells == expected


def test_truncated_record_is_refused_never_partial(tmp_path) -> None:
    blob = tmp_path / "events.bin"
    blob.write_bytes(_doc_vector() + b"\x01\x02\x03")  # 3-byte tail fragment
    with pytest.raises(events_source.BinaryEventsError):
        read_events_bin(blob)


def test_multi_record_input_keeps_input_order(tmp_path) -> None:
    second = bytearray(_doc_vector())
    second[8:12] = (4).to_bytes(4, "little")  # seq=4, everything else equal
    blob = tmp_path / "events.bin"
    blob.write_bytes(bytes(second) + _doc_vector())  # deliberately seq 4 then 3
    records = read_events_bin(blob)
    assert [record["seq"] for record in records] == [4, 3]  # no re-sort, no repair


def test_read_events_csv_normalizes_to_the_same_cell_shape(fixture_session) -> None:
    rows = read_events_csv(fixture_session / "events.csv")
    assert len(rows) == 1
    assert set(rows[0]) == set(events_source.event_csv_row(decode_events.decode(_doc_vector())))
    assert rows[0]["type"] == "UserMarked"
    assert rows[0]["payload_hex"] == "deadbeef00010203"


def test_truncated_csv_event_row_is_refused(tmp_path) -> None:
    bad = tmp_path / "events.csv"
    bad.write_text("seq,t_start_ms\n3\n", encoding="utf-8")
    with pytest.raises(events_source.BinaryEventsError):
        read_events_csv(bad)
