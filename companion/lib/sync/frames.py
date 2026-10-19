"""SYNC_PROTOCOL.md wire format (F20 spec §7/§8), both directions.

Offset-based, little-endian explicit mirror of the frozen contract v1: CURSOR
(§3.1, phone→device), STATUS (§3.2, device→phone), CHUNK (§3.3, device→phone).
Anything §4.7 drops (wrong length, bad CRC, unknown version/type) raises
`FrameError` so the caller counts it and never crashes (spec §10). The 48 B
Event record is *not* redefined here — it is the canonical `bw::Event` byte
image (§3.4 / CR-002), consumed by offset via `transport.codec.decode_event48`.
"""

from __future__ import annotations

import struct
from collections.abc import Sequence
from dataclasses import dataclass

# --- spec §8 constants (name ↔ SYNC_PROTOCOL.md; one tag + why each) ----------

# [DERIVED] SYNC §5: first byte of every control frame; decode drops mismatches.
SYNC_PROTOCOL_VERSION = 1
# [DERIVED] SYNC §3.1/§3.2 type byte (same value, disambiguated by channel/direction).
FRAME_TYPE_CURSOR = 0x01
FRAME_TYPE_STATUS = 0x01
# [DERIVED] SYNC §3.1 + F20 D5: reserved raw-window request — recognized, dropped.
FRAME_TYPE_RESERVED_RAW_WINDOW = 0x02
# [DERIVED] SYNC §3.1/§3.2 layout sums; exact-length decode guards.
CURSOR_FRAME_BYTES = 12
STATUS_FRAME_BYTES = 11
# [DERIVED] SYNC §3.4 / CR-002: canonical `bw::Event` schema v1 byte image.
EVENT_RECORD_BYTES = 48
# [DERIVED] SYNC §3.3 framing (2 B chunk_no + 2 B CRC16); 52 B min, 252 B max
# (= CR-019 `kMaxSyncFrameBytes` = ATT MTU 255 cap − 3).
CHUNK_OVERHEAD_BYTES = 4
MIN_CHUNK_FRAME_BYTES = 2 + EVENT_RECORD_BYTES + 2  # 52: exactly one whole record
MAX_CHUNK_FRAME_BYTES = 252
# [DERIVED] (252 − 4) / 48 — encode-side guard (fake device + golden vectors).
MAX_RECORDS_PER_CHUNK = 5
# [DERIVED] D9 contiguity-gate hold buffer: device in-flight worst case is
# kMaxUnackedChunks 4 x MAX_RECORDS_PER_CHUNK 5 = 20 records ahead of a hole;
# 64 ~= 3x headroom. Overflow drops held + counts (client.py, spec §10).
MAX_HELD_RECORDS = 64
# [HW] SYNC §3.3 CRC-16/CCITT-FALSE parameters (V1 check value pins the code).
CRC16_CCITT_FALSE_POLY = 0x1021
CRC16_CCITT_FALSE_INIT = 0xFFFF
# [DERIVED] SYNC §3.2 flag bits; reserved bits 4–7 ignored, never surfaced.
STATUS_FLAG_LINK_UP = 0x01
STATUS_FLAG_STREAMING = 0x02
STATUS_FLAG_MTU_TOO_LOW = 0x04
STATUS_FLAG_TRANSFER_ABORTED = 0x08


class FrameError(ValueError):
    """Any §4.7 drop condition: bad length, bad CRC, unknown version/type."""


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    """CRC-16/CCITT-FALSE (SYNC §3.3): poly 0x1021, init 0xFFFF, no reflect,
    xorout 0x0000. Bitwise — no lookup table (prototype rate, saves flash/RAM)."""
    crc = CRC16_CCITT_FALSE_INIT
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC16_CCITT_FALSE_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# --- CURSOR (§3.1, phone → device) --------------------------------------------

_CURSOR_STRUCT = struct.Struct("<BBIHI")  # ver, type, from_seq u32, max u16, ack u32


def encode_cursor(from_seq: int, max_events: int = 0, ack_seq: int = 0) -> bytes:
    """CURSOR frame; from_seq 0 = no request, max 0 = uncapped, ack 0 = no ack.
    All-zero body (from/max/ack) is the documented status query (§3.1)."""
    if not 0 <= from_seq <= 0xFFFFFFFF:
        raise FrameError(f"cursor from_seq out of u32 range: {from_seq}")
    if not 0 <= max_events <= 0xFFFF:
        raise FrameError(f"cursor max out of u16 range: {max_events}")
    if not 0 <= ack_seq <= 0xFFFFFFFF:
        raise FrameError(f"cursor ack_seq out of u32 range: {ack_seq}")
    return _CURSOR_STRUCT.pack(
        SYNC_PROTOCOL_VERSION, FRAME_TYPE_CURSOR, from_seq, max_events, ack_seq
    )


def is_status_query(frame: bytes) -> bool:
    """§3.1: from_seq == 0 && max == 0 && ack_seq == 0 ⇒ device replies §3.2."""
    return len(frame) == CURSOR_FRAME_BYTES and frame[2:] == b"\x00" * 10


@dataclass(frozen=True)
class CursorFrame:
    from_seq: int
    max_events: int
    ack_seq: int

    @property
    def is_status_query(self) -> bool:
        return self.from_seq == 0 and self.max_events == 0 and self.ack_seq == 0


def decode_cursor(frame: bytes) -> CursorFrame:
    """Parse a phone→device CURSOR (used by tests and the scripted fake device)."""
    if len(frame) != CURSOR_FRAME_BYTES:
        raise FrameError(f"cursor: expected {CURSOR_FRAME_BYTES} B, got {len(frame)} B")
    version, ftype, from_seq, max_events, ack_seq = _CURSOR_STRUCT.unpack(frame)
    if version != SYNC_PROTOCOL_VERSION:
        raise FrameError(f"cursor: unknown version {version}")
    if ftype == FRAME_TYPE_RESERVED_RAW_WINDOW:
        raise FrameError("cursor: reserved type 0x02 (raw-window request, F20 §12)")
    if ftype != FRAME_TYPE_CURSOR:
        raise FrameError(f"cursor: unknown type 0x{ftype:02x}")
    return CursorFrame(from_seq=from_seq, max_events=max_events, ack_seq=ack_seq)


# --- STATUS (§3.2, device → phone) --------------------------------------------

_STATUS_STRUCT = struct.Struct("<BBIIB")  # ver, type, last_seq u32, last_synced u32, flags


def encode_status(last_seq: int, last_synced: int, flags: int) -> bytes:
    """STATUS indication frame (device→phone, Sync Control)."""
    return _STATUS_STRUCT.pack(
        SYNC_PROTOCOL_VERSION, FRAME_TYPE_STATUS, last_seq, last_synced, flags
    )


@dataclass(frozen=True)
class SyncStatusFlags:
    link_up: bool = False
    streaming: bool = False
    mtu_too_low: bool = False
    transfer_aborted: bool = False


@dataclass(frozen=True)
class StatusFrame:
    last_seq: int
    last_synced: int
    flags: SyncStatusFlags


def decode_status(frame: bytes) -> StatusFrame:
    """Parse a STATUS indication; reserved flag bits 4–7 are ignored (§3.2)."""
    if len(frame) != STATUS_FRAME_BYTES:
        raise FrameError(f"status: expected {STATUS_FRAME_BYTES} B, got {len(frame)} B")
    version, ftype, last_seq, last_synced, raw_flags = _STATUS_STRUCT.unpack(frame)
    if version != SYNC_PROTOCOL_VERSION:
        raise FrameError(f"status: unknown version {version}")
    if ftype != FRAME_TYPE_STATUS:
        raise FrameError(f"status: unknown type 0x{ftype:02x}")
    return StatusFrame(
        last_seq=last_seq,
        last_synced=last_synced,
        flags=SyncStatusFlags(
            link_up=bool(raw_flags & STATUS_FLAG_LINK_UP),
            streaming=bool(raw_flags & STATUS_FLAG_STREAMING),
            mtu_too_low=bool(raw_flags & STATUS_FLAG_MTU_TOO_LOW),
            transfer_aborted=bool(raw_flags & STATUS_FLAG_TRANSFER_ABORTED),
        ),
    )


# --- CHUNK (§3.3, device → phone) ---------------------------------------------

_CHUNK_NO_STRUCT = struct.Struct("<H")


def encode_chunk(chunk_no: int, records: Sequence[bytes]) -> bytes:
    """Chunk frame: counter + whole 48 B records + trailing CRC16. Records are
    never split (§3.3); encode is used by golden-vector tests and the fake device."""
    if not records:
        raise FrameError("chunk: at least one record required")
    if len(records) > MAX_RECORDS_PER_CHUNK:
        raise FrameError(f"chunk: {len(records)} records > MAX_RECORDS_PER_CHUNK")
    if not 0 <= chunk_no <= 0xFFFF:
        raise FrameError(f"chunk: chunk_no out of u16 range: {chunk_no}")
    body = _CHUNK_NO_STRUCT.pack(chunk_no)
    for record in records:
        if len(record) != EVENT_RECORD_BYTES:
            raise FrameError(f"chunk: record is {len(record)} B, expected {EVENT_RECORD_BYTES} B")
        body += record
    if len(body) + 2 > MAX_CHUNK_FRAME_BYTES:
        raise FrameError(f"chunk: frame would be {len(body) + 2} B > {MAX_CHUNK_FRAME_BYTES} B")
    return body + struct.pack("<H", crc16_ccitt_false(body))


@dataclass(frozen=True)
class ChunkFrame:
    chunk_no: int
    records: tuple[bytes, ...]


def decode_chunk(frame: bytes) -> ChunkFrame:
    """Validate and split a chunk: length rules, then CRC16 over [0, len−2)."""
    if len(frame) < MIN_CHUNK_FRAME_BYTES or len(frame) > MAX_CHUNK_FRAME_BYTES:
        raise FrameError(
            f"chunk: length {len(frame)} B outside "
            f"[{MIN_CHUNK_FRAME_BYTES}, {MAX_CHUNK_FRAME_BYTES}]"
        )
    if (len(frame) - CHUNK_OVERHEAD_BYTES) % EVENT_RECORD_BYTES != 0:
        raise FrameError(
            f"chunk: payload {len(frame) - CHUNK_OVERHEAD_BYTES} B is not whole records"
        )
    (chunk_no,) = _CHUNK_NO_STRUCT.unpack_from(frame)
    (crc_expected,) = struct.unpack_from("<H", frame, len(frame) - 2)
    if crc16_ccitt_false(memoryview(frame)[:-2]) != crc_expected:
        raise FrameError("chunk: CRC16 mismatch (SYNC §3.3)")
    payload = frame[2 : len(frame) - 2]
    records = tuple(
        bytes(payload[i : i + EVENT_RECORD_BYTES])
        for i in range(0, len(payload), EVENT_RECORD_BYTES)
    )
    return ChunkFrame(chunk_no=chunk_no, records=records)
