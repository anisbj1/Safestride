"""Shared builders for the F20 sync suite.

- `make_event48`: the canonical 48 B `bw::Event` byte image (SYNC §3.4 / CR-002
  offsets, little-endian) — a builder for tests, never a second contract.
- `RecordingTransport`: F21's `FakeTransport` subclass that records start_notify
  and write calls in order (F21 files are never edited).
"""

from __future__ import annotations

import asyncio
import struct
from collections.abc import Coroutine
from typing import Any, TypeVar

from companion.transport.fake_transport import FakeTransport

_R = TypeVar("_R")


class FakeClock:
    """Deterministic monotonic clock — no wall time, no sleeps (spec §9)."""

    def __init__(self, start_ms: int = 1_000_000) -> None:
        self.t = start_ms

    def now_ms(self) -> int:
        return self.t

    def advance(self, ms: int) -> None:
        self.t += ms


def run(coro: Coroutine[Any, Any, _R]) -> _R:
    """Drive any async seam with the stdlib runner — no pytest-asyncio (spec §7)."""
    return asyncio.run(coro)

# SYNC §3.4 offsets: Q t_start_ms | I seq | I boot_id | I duration_ms |
# I t_start_unix | f confidence | I reasons | 8s payload | H quality |
# B type | B label | B context | B alertness_at_start | B detector_id | B schema
_EVENT48_STRUCT = struct.Struct("<QIIIIfI8sHBBBBBB")
assert _EVENT48_STRUCT.size == 48  # pins the layout against the contract

# SYNC §6 golden vectors (re-declared from the frozen contract, never invented).
V1_CHECK_STRING = b"123456789"
V1_CHECK_VALUE = 0x29B1
V2_CURSOR_HEX = "010101000000400000000000"  # from=1 max=64 ack=0
V3_STATUS_HEX = "0101640000002000000001"  # last=100 synced=32 flags=0x01
# V4: 1-event chunk (52 B), fixture record per the contract's V4 annotation.
V4_FRAME_HEX = (
    "0100"  # chunk_no 1
    "E803000000000000"  # t_start_ms = 1000
    "07000000"  # seq = 7
    "03000000"  # boot_id = 3
    "FA000000"  # duration_ms = 250
    "00000000"  # t_start_unix = 0
    "0000003F"  # confidence = 0.5f
    "00000000"  # reasons = 0
    "0000000000000000"  # payload[8] = 0
    "0000"  # quality = {Good, Good}
    "01"  # type = DrowsinessEvent(1)
    "00"  # label = Unlabeled(0)
    "00"  # context = Unknown(0)
    "00"  # alertness_at_start = Unknown(0)
    "09"  # detector_id = 9
    "01"  # schema_version = 1
    "A9D2"  # CRC16/CCITT-FALSE = 0xD2A9, LE
)


def make_event48(
    seq: int,
    *,
    t_start_ms: int | None = None,
    boot_id: int = 1,
    duration_ms: int = 100,
    t_start_unix: int = 0,
    confidence: float = 0.0,
    reasons: int = 0,
    payload: bytes = b"",
    quality: int = 0,
    type: int = 0,  # noqa: A002 — mirrors the contract's field name
    label: int = 0,
    context: int = 0,
    alertness_at_start: int = 0,
    detector_id: int = 0,
    schema_version: int = 1,
) -> bytes:
    """Build one 48 B Event record by the contract's offsets (SYNC §3.4)."""
    if t_start_ms is None:
        t_start_ms = seq * 1000  # deterministic, monotonic in seq
    payload8 = bytes(payload).ljust(8, b"\x00")[:8]
    return _EVENT48_STRUCT.pack(
        t_start_ms,
        seq,
        boot_id,
        duration_ms,
        t_start_unix,
        confidence,
        reasons,
        payload8,
        quality,
        type,
        label,
        context,
        alertness_at_start,
        detector_id,
        schema_version,
    )


def record_seq(raw: bytes) -> int:
    """seq field by contract offset (8..12, LE) — no struct cast anywhere."""
    return int.from_bytes(raw[8:12], "little")


class RecordingTransport(FakeTransport):
    """F21's FakeTransport + ordered event log for subscription/write-order CTs."""

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self.events: list[tuple[str, str]] = []

    async def start_notify(self, char: str, cb) -> None:  # type: ignore[override]
        self.events.append(("start_notify", char))
        await super().start_notify(char, cb)

    async def write(self, char: str, data: bytes) -> None:  # type: ignore[override]
        self.events.append(("write", char))
        await super().write(char, data)
