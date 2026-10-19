"""Golden byte vectors, verbatim from the contracts' own test-vector sections
(BLE_GATT.md §6, SYNC_PROTOCOL.md §6) + a canonical 48 B record builder.

Drift in either direction fails these tests: if a contract layout moves, the
vectors no longer decode/round-trip until they are re-imported deliberately
(spec §9 strategy — the contracts' vectors, not hand-made data).
"""

from __future__ import annotations

import struct

# --- BLE_GATT.md §6 -----------------------------------------------------------

# V1 — Live Status: Drowsy/Worn/Sitting, quality Degraded, Day/Normal, in_state 45 s,
# confidence 0.5 (=50%), reasons 0x12345678.
V1_LIVE_STATUS = bytes.fromhex("01 03 01 02 01 00 00 2D 00 32 78 56 34 12")

# V2 — Heart Rate: uint8 72 bpm / uint16 300 bpm.
V2_HR_UINT8_72 = bytes.fromhex("00 48")
V2_HR_UINT16_300 = bytes.fromhex("01 2C 01")

# V3 — Command MarkEvent arg0=0x01020304 arg1=0xAABBCCDD (frame builder cross-check).
V3_COMMAND_MARK_EVENT = bytes.fromhex("04 04 03 02 01 DD CC BB AA")

# V6 — first 16 bytes of an Event record: t_start_ms=1000, seq=42, boot_id=7.
V6_EVENT_FIRST_16 = bytes.fromhex("E8 03 00 00 00 00 00 00 2A 00 00 00 07 00 00 00")

# --- SYNC_PROTOCOL.md §6 ------------------------------------------------------

# V4 — 1-event CHUNK, 52 B: chunk_no 1 + the canonical 48 B record + CRC-16 0xD2A9.
# Fixture: t_start_ms=1000, seq=7, boot_id=3, duration=250, unix=0, conf=0.5,
# type=DrowsinessEvent(1), detector_id=9, schema_version=1.
V4_CHUNK_1_EVENT = bytes.fromhex(
    "01 00 E8 03 00 00 00 00 00 00 07 00 00 00 03 00"
    "00 00 FA 00 00 00 00 00 00 00 00 00 00 3F 00 00"
    "00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00"
    "09 01 A9 D2"
)

# The record image inside the V4 chunk (bytes 2..50).
V4_RECORD = V4_CHUNK_1_EVENT[2:50]


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0; SYNC §3.3).
    V1 check value: "123456789" → 0x29B1."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# Canonical 48 B Event layout (SYNC §3.4), little-endian, no padding:
# t_start_ms u64 | seq u32 | boot_id u32 | duration_ms u32 | t_start_unix u32 |
# confidence f32 | reasons u32 | payload[8] | quality u16 | 6x u8
_EVENT48 = struct.Struct("<QIIIIfI8sHBBBBBB")
assert _EVENT48.size == 48


def make_event_bytes(
    *,
    t_start_ms: int = 1000,
    seq: int = 7,
    boot_id: int = 3,
    duration_ms: int = 250,
    t_start_unix: int = 0,
    confidence: float = 0.5,
    reasons: int = 0,
    payload: bytes = bytes(8),
    quality: int = 0,  # SourceQuality: low byte imu, high byte ppg (both Good here)
    type: int = 1,  # DrowsinessEvent
    label: int = 0,  # Unlabeled
    context: int = 0,  # Unknown
    alertness_at_start: int = 0,  # Unknown
    detector_id: int = 9,
    schema_version: int = 1,
) -> bytes:
    """Build a canonical 48 B event record; defaults mirror the V4 fixture record
    (asserted by test_codec_event48), so overrides are the only thing to think about."""
    return _EVENT48.pack(
        t_start_ms,
        seq,
        boot_id,
        duration_ms,
        t_start_unix,
        confidence,
        reasons,
        payload,
        quality,
        type,
        label,
        context,
        alertness_at_start,
        detector_id,
        schema_version,
    )


def split_chunk(frame: bytes) -> list[bytes]:
    """Split a SYNC §3.3 chunk frame into its 48 B records (frame_len % 48 == 4)."""
    if len(frame) < 4 or (len(frame) - 4) % 48 != 0:
        raise ValueError(f"bad chunk frame length {len(frame)}")
    payload = frame[2:-2]
    return [payload[i : i + 48] for i in range(0, len(payload), 48)]
