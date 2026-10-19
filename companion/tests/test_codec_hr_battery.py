"""§9 test 3 — Heart Rate (both widths) and Battery codecs, BLE_GATT §6 V2/§4.5."""

from __future__ import annotations

import pytest
from companion.transport.codec import CodecError, decode_battery, decode_hr
from vectors import V2_HR_UINT8_72, V2_HR_UINT16_300


def test_hr_uint8_golden():
    assert decode_hr(V2_HR_UINT8_72) == 72


def test_hr_uint16_golden():
    assert decode_hr(V2_HR_UINT16_300) == 300


def test_hr_rejects_malformed_widths_and_flags():
    with pytest.raises(CodecError):  # 16-bit flag but uint8-width image
        decode_hr(b"\x01\x48")
    with pytest.raises(CodecError):  # uint8 flag but 16-bit-width image
        decode_hr(b"\x00\x48\x01")
    with pytest.raises(CodecError):  # no other flag bits are ever set (§4.2)
        decode_hr(b"\x80\x48")
    with pytest.raises(CodecError):  # wrong length
        decode_hr(b"\x00")


def test_battery_is_a_percentage():
    assert decode_battery(bytes([87])).percent == 87
    assert decode_battery(bytes([0])).percent == 0
    assert decode_battery(bytes([100])).percent == 100


def test_battery_0xff_is_unknown_never_a_percentage():
    assert decode_battery(b"\xff").percent is None  # BLE_GATT §4.5 / audit AUD-7


def test_battery_out_of_range_is_rejected_not_coerced():
    with pytest.raises(CodecError):  # 101..254 are contract-invalid, not "unknown"
        decode_battery(bytes([200]))
    with pytest.raises(CodecError):
        decode_battery(b"")
