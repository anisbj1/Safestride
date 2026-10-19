"""§9 test 1 — event48 codec pinned by BLE_GATT §6 V6 + SYNC §6 V4 contract vectors."""

from __future__ import annotations

import pytest
from companion.data import db
from companion.data.event_repository import EventRepository
from companion.data.models import EVENT_SCHEMA_VERSION
from companion.transport.codec import (
    EVENT_RECORD_BYTES,
    CodecError,
    decode_event48,
    encode_event48,
)
from vectors import (
    V4_CHUNK_1_EVENT,
    V4_RECORD,
    V6_EVENT_FIRST_16,
    crc16_ccitt_false,
    make_event_bytes,
    split_chunk,
)


def test_v4_chunk_is_52_bytes_with_a_valid_crc():
    assert len(V4_CHUNK_1_EVENT) == 52
    assert (len(V4_CHUNK_1_EVENT) - 4) % EVENT_RECORD_BYTES == 0  # whole records only
    assert crc16_ccitt_false(V4_CHUNK_1_EVENT[:-2]) == 0xD2A9  # SYNC §6 V1 CRC
    assert V4_CHUNK_1_EVENT[:2] == b"\x01\x00"  # chunk_no 1
    assert split_chunk(V4_CHUNK_1_EVENT) == [V4_RECORD]


def test_v6_prefix_matches_the_canonical_layout():
    # BLE_GATT §6 V6 (t_start_ms=1000, seq=42, boot_id=7) shares the canonical
    # offsets with SYNC §3.4: the same event built through the contract must
    # reproduce those exact 16 bytes.
    record = make_event_bytes(t_start_ms=1000, seq=42, boot_id=7)
    assert record[:16] == V6_EVENT_FIRST_16


def test_v4_record_decodes_to_the_fixture_fields():
    rec = decode_event48(V4_RECORD)
    assert rec.t_start_ms == 1000
    assert rec.seq == 7
    assert rec.boot_id == 3
    assert rec.duration_ms == 250
    assert rec.t_start_unix == 0
    assert rec.confidence == pytest.approx(0.5)
    assert rec.reasons == 0
    assert rec.payload == bytes(8)
    assert rec.quality == 0  # SourceQuality {Good, Good}
    assert rec.type == 1  # DrowsinessEvent
    assert rec.label == 0  # Unlabeled
    assert rec.context == 0  # Unknown
    assert rec.alertness_at_start == 0
    assert rec.detector_id == 9
    assert rec.schema_version == EVENT_SCHEMA_VERSION
    assert rec.supported
    assert rec.raw == V4_RECORD


def test_encode_round_trips_the_contract_fixture():
    assert encode_event48(decode_event48(V4_RECORD)) == V4_RECORD
    assert make_event_bytes() == V4_RECORD  # builder defaults mirror the V4 fixture


@pytest.mark.parametrize("bad_len", [1, 47, 49, 52, 53])
def test_wrong_length_is_rejected_never_guessed(bad_len: int):
    with pytest.raises(CodecError):
        decode_event48(bytes(bad_len))


def test_unknown_schema_is_stored_raw_and_flagged_unsupported(tmp_path):
    raw = make_event_bytes(schema_version=2)
    rec = decode_event48(raw)
    assert not rec.supported  # decoded by offsets but flagged — never rendered as data
    conn = db.open_db(tmp_path / "unsupported.sqlite3")
    try:
        repo = EventRepository(conn)
        assert repo.upsert("dev1", [raw]) == 1
        row = conn.execute(
            "SELECT raw, schema_version FROM events WHERE device_id='dev1'"
        ).fetchone()
        assert bytes(row["raw"]) == raw  # raw blob kept verbatim for later re-decode (D5)
        assert row["schema_version"] == 2
    finally:
        conn.close()
