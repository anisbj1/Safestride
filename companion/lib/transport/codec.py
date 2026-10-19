"""Pure payload codecs by contract offsets (BLE_GATT.md §4, SYNC_PROTOCOL.md §3.4).

No I/O: every function is bytes → value or value → bytes, host-tested against the
contracts' own golden vectors (spec §9). Unknown version bytes and malformed lengths
are rejected with CodecError — never coerced to a known layout (BLE_GATT §1).
"""

from __future__ import annotations

import struct

from companion.data.models import Battery, EventRecord, LiveStatus

# [HW] Canonical 48 B Event image length (CR-002 / BLE_GATT §4.6, spec §8).
EVENT_RECORD_BYTES = 48
# [HW] Live Status image length, version 1 (BLE_GATT §4.1, spec §8).
LIVE_STATUS_BYTES = 14
# [HW] Standard HR Measurement image width: 2 B (uint8 bpm) or 3 B (uint16 bpm),
# flag bit0 selects (BLE_GATT §4.2).
HR_MIN_BYTES = 2
HR_MAX_BYTES = 3
# [HW] Command image length: type u8 + arg0 u32 LE + arg1 u32 LE (BLE_GATT §4.3).
COMMAND_BYTES = 9
# [HW] Battery Level image: 1 byte, 0..100 percent, 0xFF = unknown (BLE_GATT §4.5).
BATTERY_LEVEL_BYTES = 1
BATTERY_UNKNOWN_RAW = 0xFF
BATTERY_MAX_PERCENT = 100
# [HW] Serial Number carries device_id: 12 lowercase hex chars (BLE_GATT §2).
DEVICE_ID_HEX_CHARS = 12

# Event48 layout (SYNC §3.4 / data_decision.md §4.5), little-endian, no padding:
# t_start_ms u64 | seq u32 | boot_id u32 | duration_ms u32 | t_start_unix u32 |
# confidence f32 | reasons u32 | payload[8] | quality u16 (imu, ppg) |
# type | label | context | alertness_at_start | detector_id | schema_version
_EVENT48 = struct.Struct("<QIIIIfI8sHBBBBBB")
assert _EVENT48.size == EVENT_RECORD_BYTES  # contract-pinned length (CR-002)

# Live Status v1 (BLE_GATT §4.1): ver u8 | alertness u8 | wear u8 | activity u8 |
# quality u8 | mode u8 | power u8 | in_state_s u16 | confidence_pct u8 | reasons u32
_LIVE_STATUS_V1 = struct.Struct("<BBBBBBBHBI")
assert _LIVE_STATUS_V1.size == LIVE_STATUS_BYTES

# [HW] Live Status version byte = 1 (BLE_GATT §1/§4.1); unknown → reject, never guess.
LIVE_STATUS_VERSION = 1


class CodecError(ValueError):
    """Malformed or unknown-version payload — rejected, never coerced (BLE_GATT §1)."""


def decode_live_status(data: bytes) -> LiveStatus | None:
    """Decode the 14 B Live Status image. 0 bytes = "no snapshot yet" → None
    (BLE_GATT §5: the value stays empty until the device's first StateSnapshot)."""
    if not data:
        return None
    if len(data) != LIVE_STATUS_BYTES:
        raise CodecError(f"live status: expected {LIVE_STATUS_BYTES} B, got {len(data)} B")
    (ver, alertness, wear, activity, quality, mode, power, in_state_s, conf_pct, reasons) = (
        _LIVE_STATUS_V1.unpack(data)
    )
    if ver != LIVE_STATUS_VERSION:
        raise CodecError(
            f"live status: unknown version byte {ver} (expected {LIVE_STATUS_VERSION})"
        )
    return LiveStatus(
        alertness=alertness,
        wear=wear,
        activity=activity,
        quality=quality,
        mode=mode,
        power=power,
        in_state_s=in_state_s,
        confidence_pct=conf_pct,
        reasons=reasons,
    )


# [HW] HR Measurement flag bit0: 1 = bpm is uint16, 0 = uint8; no other bits are set
# (BLE_GATT §4.2).
_HR_FLAG_UINT16 = 0x01


def decode_hr(data: bytes) -> int:
    """Decode the standard 0x2A37 image → bpm. Malformed widths/flags are rejected."""
    if len(data) not in (HR_MIN_BYTES, HR_MAX_BYTES):
        raise CodecError(f"heart rate: expected {HR_MIN_BYTES}-{HR_MAX_BYTES} B, got {len(data)} B")
    flags = data[0]
    if flags & ~_HR_FLAG_UINT16:
        raise CodecError(f"heart rate: unexpected flag bits 0x{flags:02x}")
    if flags & _HR_FLAG_UINT16:
        if len(data) != HR_MAX_BYTES:
            raise CodecError("heart rate: 16-bit flag with a uint8-width image")
        return struct.unpack_from("<H", data, 1)[0]
    if len(data) != HR_MIN_BYTES:
        raise CodecError("heart rate: uint8 flag with a 16-bit-width image")
    return data[1]


def decode_battery(data: bytes) -> Battery:
    """Decode Battery Level: 0xFF = unknown → None; anything outside 0..100 and not
    0xFF is contract-invalid and rejected — bad data is never a percentage (BLE_GATT §4.5)."""
    if len(data) != BATTERY_LEVEL_BYTES:
        raise CodecError(f"battery: expected {BATTERY_LEVEL_BYTES} B, got {len(data)} B")
    raw = data[0]
    if raw == BATTERY_UNKNOWN_RAW:
        return Battery(percent=None)
    if raw > BATTERY_MAX_PERCENT:
        raise CodecError(f"battery: invalid level {raw} (0-100 or 0xFF only)")
    return Battery(percent=raw)


def decode_event48(data: bytes) -> EventRecord:
    """Decode the canonical 48 B Event image by contract offsets (SYNC §3.4).

    Length must be exactly 48. An unknown schema_version decodes (offsets are the
    only contract-stable fact) but yields `supported == False`: callers keep the raw
    blob and render the row as unsupported — never as data (spec §9 test 1, D5).
    """
    if len(data) != EVENT_RECORD_BYTES:
        raise CodecError(f"event: expected {EVENT_RECORD_BYTES} B, got {len(data)} B")
    (
        t_start_ms,
        seq,
        boot_id,
        duration_ms,
        t_start_unix,
        confidence,
        reasons,
        payload,
        quality,
        ev_type,
        label,
        context,
        alertness_at_start,
        detector_id,
        schema_version,
    ) = _EVENT48.unpack(data)
    return EventRecord(
        t_start_ms=t_start_ms,
        seq=seq,
        boot_id=boot_id,
        duration_ms=duration_ms,
        t_start_unix=t_start_unix,
        confidence=confidence,
        reasons=reasons,
        payload=payload,
        quality=quality,
        type=ev_type,
        label=label,
        context=context,
        alertness_at_start=alertness_at_start,
        detector_id=detector_id,
        schema_version=schema_version,
        raw=bytes(data),
    )


def encode_event48(rec: EventRecord) -> bytes:
    """Encode a record back to its canonical 48 B image (round-trip pinned by tests;
    the DB keeps the raw blob, so this exists for tooling, not for storage)."""
    return _EVENT48.pack(
        rec.t_start_ms,
        rec.seq,
        rec.boot_id,
        rec.duration_ms,
        rec.t_start_unix,
        rec.confidence,
        rec.reasons,
        rec.payload,
        rec.quality,
        rec.type,
        rec.label,
        rec.context,
        rec.alertness_at_start,
        rec.detector_id,
        rec.schema_version,
    )


def decode_serial(data: bytes) -> str:
    """Decode the Serial Number characteristic → device_id (12 lowercase hex,
    BLE_GATT §2); anything else is rejected, never normalized into an identity."""
    text = data.decode("ascii", errors="strict").strip().lower()
    if len(text) != DEVICE_ID_HEX_CHARS or any(c not in "0123456789abcdef" for c in text):
        raise CodecError(f"serial: {data!r} is not a 12-hex device_id")
    return text


def decode_revision(data: bytes) -> str:
    """Decode a DIS revision characteristic (ASCII build string, BLE_GATT §3 rows 0-2)."""
    return data.decode("ascii", errors="replace").rstrip("\x00").strip()
