"""§9 test 2 — Live Status codec pinned by BLE_GATT §6 V1."""

from __future__ import annotations

import pytest
from companion.transport.codec import CodecError, decode_live_status
from vectors import V1_LIVE_STATUS


def test_v1_golden_decodes_to_the_fixture_fields():
    status = decode_live_status(V1_LIVE_STATUS)
    assert status is not None
    assert status.alertness == 3  # Drowsy
    assert status.wear == 1  # Worn
    assert status.activity == 2  # Sitting
    assert status.quality == 1  # Degraded
    assert status.mode == 0  # Day
    assert status.power == 0  # Normal
    assert status.in_state_s == 45
    assert status.confidence_pct == 50
    assert status.reasons == 0x12345678


def test_unknown_version_byte_is_rejected_never_coerced():
    with pytest.raises(CodecError, match="version"):
        decode_live_status(b"\x02" + V1_LIVE_STATUS[1:])


def test_zero_byte_read_is_no_snapshot_yet():
    # BLE_GATT §5: the value is empty until the device's first StateSnapshot.
    assert decode_live_status(b"") is None


def test_wrong_length_is_rejected():
    with pytest.raises(CodecError):
        decode_live_status(V1_LIVE_STATUS[:-1])
    with pytest.raises(CodecError):
        decode_live_status(V1_LIVE_STATUS + b"\x00")
