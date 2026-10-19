"""CT01–CT09: SYNC_PROTOCOL.md golden vectors + malformed-frame rules (§3, §4.7)."""

from __future__ import annotations

import pytest
from companion.sync import frames

from .sync_test_support import (
    V1_CHECK_STRING,
    V1_CHECK_VALUE,
    V2_CURSOR_HEX,
    V3_STATUS_HEX,
    V4_FRAME_HEX,
    make_event48,
)


def test_CT01_crc16_ccitt_false_check_value_v1() -> None:
    assert frames.crc16_ccitt_false(V1_CHECK_STRING) == V1_CHECK_VALUE  # 0x29B1
    assert frames.CRC16_CCITT_FALSE_POLY == 0x1021
    assert frames.CRC16_CCITT_FALSE_INIT == 0xFFFF


def test_CT02_encode_cursor_golden_v2() -> None:
    assert frames.encode_cursor(1, 64, 0).hex() == V2_CURSOR_HEX
    assert len(frames.encode_cursor(1, 64, 0)) == frames.CURSOR_FRAME_BYTES == 12


def test_CT03_status_query_and_request_ack_combined_frames() -> None:
    query = frames.encode_cursor(0, 0, 0)
    assert query[2:] == b"\x00" * 10  # all-zero body (§3.1)
    assert frames.is_status_query(query)
    assert frames.decode_cursor(query) == frames.CursorFrame(0, 0, 0)
    assert frames.decode_cursor(query).is_status_query
    combined = frames.encode_cursor(5, 32, 3)  # one frame: request AND ack
    assert frames.decode_cursor(combined) == frames.CursorFrame(5, 32, 3)
    assert not frames.decode_cursor(combined).is_status_query


def test_CT04_decode_cursor_round_trip() -> None:
    for from_seq, maximum, ack in [(1, 0, 0), (7, 64, 3), (0xFFFFFFFF, 0xFFFF, 0xFFFFFFFF)]:
        assert frames.decode_cursor(frames.encode_cursor(from_seq, maximum, ack)) == (
            frames.CursorFrame(from_seq, maximum, ack)
        )


def test_CT05_encode_decode_status_golden_v3_flag_bits() -> None:
    assert frames.encode_status(100, 32, 0x01).hex() == V3_STATUS_HEX
    assert len(frames.encode_status(100, 32, 0x01)) == frames.STATUS_FRAME_BYTES == 11
    decoded = frames.decode_status(bytes.fromhex(V3_STATUS_HEX))
    assert (decoded.last_seq, decoded.last_synced) == (100, 32)
    assert decoded.flags.link_up and not decoded.flags.streaming
    assert not decoded.flags.mtu_too_low and not decoded.flags.transfer_aborted
    # all four bits mapped; reserved bits 4-7 ignored, never surfaced (§3.2)
    full = frames.decode_status(frames.encode_status(1, 1, 0x0F | 0xF0))
    assert (
        full.flags.link_up
        and full.flags.streaming
        and full.flags.mtu_too_low
        and full.flags.transfer_aborted
    )


def test_CT06_chunk_golden_v4_and_5_record_chunk() -> None:
    v4_record = make_event48(
        7, t_start_ms=1000, boot_id=3, duration_ms=250, confidence=0.5, type=1, detector_id=9
    )
    assert v4_record.hex() == V4_FRAME_HEX.lower()[4:-4]  # the record image inside V4
    encoded = frames.encode_chunk(1, [v4_record])
    assert encoded.hex() == V4_FRAME_HEX.lower()  # 52 B frame, CRC 0xD2A9 LE (§6 V4)
    decoded = frames.decode_chunk(encoded)
    assert decoded.chunk_no == 1
    assert decoded.records == (v4_record,)
    # 5-record chunk at MTU 255: 2 + 240 + 2 = 244 B (§3.3)
    five = frames.encode_chunk(2, [v4_record] * 5)
    assert len(five) == 244
    decoded5 = frames.decode_chunk(five)
    assert decoded5.chunk_no == 2
    assert len(decoded5.records) == 5 and all(len(r) == 48 for r in decoded5.records)


def test_CT07_corrupted_crc_rejected() -> None:
    frame = bytearray(frames.encode_chunk(1, [make_event48(1)]))
    frame[-1] ^= 0xFF
    with pytest.raises(frames.FrameError, match="CRC"):
        frames.decode_chunk(bytes(frame))


def test_CT08_wrong_chunk_length_rejected() -> None:
    record = make_event48(1)
    for bad in (
        b"\x01\x00" + record[:47] + b"\x00\x00",  # 51 B: below the 52 B floor
        b"\x01\x00" + record + b"\x00\x00\x00\x00",  # 56 B: not whole records
        frames.encode_chunk(1, [record]) * 5,  # 260 B: above the 252 B cap
        b"\x01\x00" + record + b"\x00",  # 51 B again with trailing junk stripped
    ):
        with pytest.raises(frames.FrameError):
            frames.decode_chunk(bad)


def test_CT09_unknown_version_reserved_type_wrong_length_dropped() -> None:
    cursor = bytearray(frames.encode_cursor(1, 64, 0))
    cursor[0] = 2  # unknown version (§5)
    with pytest.raises(frames.FrameError, match="version"):
        frames.decode_cursor(bytes(cursor))
    reserved = bytearray(frames.encode_cursor(1, 64, 0))
    reserved[1] = frames.FRAME_TYPE_RESERVED_RAW_WINDOW  # 0x02 (D5 reserved)
    with pytest.raises(frames.FrameError, match="reserved"):
        frames.decode_cursor(bytes(reserved))
    unknown_type = bytearray(frames.encode_cursor(1, 64, 0))
    unknown_type[1] = 0x7E
    with pytest.raises(frames.FrameError, match="type"):
        frames.decode_cursor(bytes(unknown_type))
    with pytest.raises(frames.FrameError, match="expected 11"):
        frames.decode_status(frames.encode_status(1, 1, 0x01)[:-1])  # 10 B STATUS
    with pytest.raises(frames.FrameError, match="type"):
        bad_type = bytearray(frames.encode_status(1, 1, 0x01))
        bad_type[1] = 0x02  # reserved type on the STATUS side too
        frames.decode_status(bytes(bad_type))
