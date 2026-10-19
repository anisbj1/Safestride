"""Contract-level 48 B Event builders for the F16 companion tests (plan §15.6).

`pack_event` encodes the canonical record by contract offsets (data_decision.md
§4.5, CR-002/CR-011: u64 t_start_ms first, all 1-byte fields last — exactly 48 B,
the same struct layout F21's `transport/codec.py` unpacks). Tests seed the DB
through F21's real `upsert`, so SYNC §4.5 wall-time resolution runs exactly as in
production; `record()` builds an in-memory `EventRecord` for the pure-aggregate
tests, stamping `wall_unix` the way the repository column would hold it.
"""

from __future__ import annotations

import struct
from dataclasses import replace

from companion.data.models import EventRecord
from companion.transport.codec import decode_event48

# [HW] Contract field order/widths (data_decision.md §4.5) — mirrors codec._EVENT48;
# spaces keep the field list readable (struct ignores them).
_EVENT48 = struct.Struct("< Q I I I I f I 8s H B B B B B B")
assert _EVENT48.size == 48  # [HW] CR-002: the record is exactly 48 B, zero padding


def pack_event(
    *,
    seq: int,
    boot_id: int = 1,
    t_start_ms: int = 0,
    duration_ms: int = 0,
    t_start_unix: int = 0,
    confidence: float = 0.0,
    reasons: int = 0,
    payload: bytes = bytes(8),
    quality: int = 0,
    type: int = 0,
    label: int = 0,
    context: int = 0,
    alertness_at_start: int = 0,
    detector_id: int = 0,
    schema_version: int = 1,
) -> bytes:
    """One canonical 48 B event image (contract offsets; unspecified fields stay zero)."""
    if len(payload) != 8:
        raise ValueError(f"payload must be exactly 8 B, got {len(payload)}")
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


def record(*, wall_unix: int | None = None, **kwargs) -> EventRecord:
    """Decode-level EventRecord for the pure-aggregate tests.

    `wall_unix` defaults to `t_start_unix` (what the repository stores at insert
    for explicit-unix events); pass 0 explicitly for unknown-wall-time rows.
    """
    decoded = decode_event48(pack_event(**kwargs))
    if wall_unix is None:
        wall_unix = decoded.t_start_unix
    return replace(decoded, wall_unix=wall_unix)
