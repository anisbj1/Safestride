"""Single events adapter for export (F28 D18): CSV reader + binary reader.

Events reach export in two shapes today — the text ``events.csv`` a capture
produced, and packed 48-byte ``bw::Event`` records (sync/BLE/SQLite path
later). Both readers here normalize to the same row-dict shape (keys =
DATASET_FORMAT.md §3 ``events.csv`` columns), and the binary layout is
owned solely by ``tools/decode_events.py`` — this module never restates the
byte layout; it only delegates (CR-002/CR-011, canonical vector
``F28-EVENT-VECTOR-48B`` in DATASET_FORMAT.md §5).
"""

from __future__ import annotations

import sys
from pathlib import Path

if __package__ in (None, ""):  # allow `python tools/export/events_source.py ...`
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import decode_events  # noqa: E402 - after the path shim above

# CR-002/CR-011 Event record size, canonical vector CR-012 (DATASET_FORMAT.md
# §5). NOT a second definition of the layout: read_events_bin slices the blob
# into records of this size and hands every byte to decode_events.decode;
# tests/test_events_bin.py asserts this constant equals
# decode_events.EVENT_SIZE, so a drift on either side fails there.
# [DERIVED] spec §16 constants table.
EVENT_RECORD_BYTES = 48


class BinaryEventsError(ValueError):
    """A binary event blob that cannot be read as whole 48-byte records."""


def read_events_csv(path: Path | str) -> list[dict[str, str]]:
    """Read an ``events.csv`` into row dicts of string cells (verbatim)."""
    rows: list[dict[str, str]] = []
    text = Path(path).read_text(encoding="utf-8")
    lines = [line for line in text.split("\n") if line and not line.startswith("#")]
    if not lines:
        return rows
    columns = lines[0].split(",")
    for number, line in enumerate(lines[1:], start=2):
        cells = line.split(",")
        if len(cells) != len(columns):
            raise BinaryEventsError(
                f"{path}:{number}: truncated event row ({len(cells)} of "
                f"{len(columns)} fields) — never repaired, only refused (§10)"
            )
        rows.append(dict(zip(columns, cells, strict=True)))
    return rows


def read_events_bin(path: Path | str) -> list[dict[str, object]]:
    """Read packed 48-byte Event records via decode_events (D18: delegate).

    A trailing fragment (< EVENT_RECORD_BYTES) is refused whole — no partial
    row is ever produced (mirror of the capture-side "never repaired" rule).
    """
    blob = Path(path).read_bytes()
    events: list[dict[str, object]] = []
    offset = 0
    while offset < len(blob):
        record = blob[offset : offset + EVENT_RECORD_BYTES]
        if len(record) != EVENT_RECORD_BYTES:
            raise BinaryEventsError(
                f"{path}: trailing {len(record)}-byte fragment after "
                f"{offset} whole {EVENT_RECORD_BYTES}-byte records — refusing "
                "a partial event row (never repaired, §10)"
            )
        events.append(decode_events.decode(record))
        offset += EVENT_RECORD_BYTES
    return events


def event_csv_row(record: dict[str, object]) -> dict[str, str]:
    """Normalize a decoded event record to the events.csv cell shape (D18)."""
    confidence = record["confidence"]
    return {
        "seq": str(record["seq"]),
        "t_start_ms": str(record["t_start_ms"]),
        "boot_id": str(record["boot_id"]),
        "duration_ms": str(record["duration_ms"]),
        "t_start_unix": str(record["t_start_unix"]),
        "confidence": f"{float(confidence):.3f}",
        "reasons": str(record["reasons"]),
        "payload_hex": str(record["payload_hex"]),
        "quality_imu": str(record["quality_imu"]),
        "quality_ppg": str(record["quality_ppg"]),
        "type": str(record["type"]),
        "label": str(record["label"]),
        "activity": str(record["activity"]),
        "alertness": str(record["alertness"]),
        "detector_id": str(record["detector_id"]),
        "schema_version": str(record["schema_version"]),
    }
