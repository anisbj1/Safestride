"""§9 test 4 — SetTime command frame per CR-021 (accepted, data_decision.md §4.6):
arg0 = unix_seconds u32 LE, arg1 = 0 reserved; BLE_GATT §4.3 9-byte image."""

from __future__ import annotations

import struct

import pytest
from companion.transport.commands import COMMAND_TYPE_SET_TIME, build_set_time
from vectors import V3_COMMAND_MARK_EVENT


def test_set_time_golden_frame_layout():
    frame = build_set_time(0x01020304)
    assert len(frame) == 9  # BLE_GATT §4.3 Command image
    assert frame == bytes.fromhex("0B 04 03 02 01 00 00 00 00")
    assert frame[0] == COMMAND_TYPE_SET_TIME == 11  # append-only CommandType index
    assert struct.unpack_from("<I", frame, 1)[0] == 0x01020304  # arg0 = unix_seconds
    assert struct.unpack_from("<I", frame, 5)[0] == 0  # arg1 reserved, CR-021


def test_set_time_typical_value():
    assert build_set_time(1760000000) == b"\x0b" + struct.pack("<I", 1760000000) + b"\x00" * 4


def test_set_time_rejects_out_of_u32_range():
    with pytest.raises(ValueError):
        build_set_time(-1)
    with pytest.raises(ValueError):
        build_set_time(0x1_0000_0000)


def test_command_layout_matches_the_mark_event_vector_shape():
    # BLE_GATT §6 V3: type u8 + arg0 u32 LE + arg1 u32 LE — same 9-byte skeleton.
    assert len(V3_COMMAND_MARK_EVENT) == 9
    assert V3_COMMAND_MARK_EVENT[0] == 4  # MarkEvent
    assert struct.unpack_from("<I", V3_COMMAND_MARK_EVENT, 1)[0] == 0x01020304
    assert struct.unpack_from("<I", V3_COMMAND_MARK_EVENT, 5)[0] == 0xAABBCCDD
