"""Decode a 48-byte ``bw::Event`` record (docs/contracts/DATASET_FORMAT.md §5).

The firmware never serialises Event: it is memcpy'd as-is — little-endian,
no padding, offsets pinned by ``firmware/lib/bw_core/.../contract_checks.h``
(CR-002). This module unpacks those bytes for offline analysis, for
``tools/replay`` and for the future BLE export path (F19/F20). The Python
test vector here and the C++ vector test
(``firmware/test/bw_research/test_bw_research/test_event_layout.cpp``) pin
the same bytes, so a layout drift on either side fails both.

Usage:
    python tools/decode_events.py events.bin          # binary file, JSON per line
    python tools/decode_events.py --hex e5cd5b07...   # one record as hex text
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

EVENT_SIZE = 48  # bw::Event::kEventBytes
EVENT_SCHEMA_VERSION = 1  # bw::Event::kSchemaVersion

# Field order is load-bearing (CR-002, events.h): TimeMs first, all 1-byte
# fields last, zero padding. '<' = little-endian on both ends.
_EVENT_STRUCT = struct.Struct("<QIIIIfI8sBBBBBBBB")

QUALITY_NAMES = ("Good", "Degraded", "Insufficient")
EVENT_TYPE_NAMES = (
    "SleepEpisode",
    "DrowsinessEvent",
    "SuspectedCataplexy",
    "UserMarked",
    "AlertRaised",
    "SensorFault",
    "DeviceNotWorn",
    "BatteryWarning",
    "NapPlanned",
    "NapCompleted",
    "TimeSync",
    "Boot",
)
USER_LABEL_NAMES = ("Unlabeled", "Confirmed", "Rejected")
ACTIVITY_NAMES = (
    "Unknown",
    "Stationary",
    "Sitting",
    "Standing",
    "Walking",
    "Running",
    "LyingResting",
    "InactiveProlonged",
)
ALERTNESS_NAMES = (
    "Unknown",
    "NotWorn",
    "Awake",
    "Drowsy",
    "HighDrowsinessRisk",
    "PossibleSleep",
)
# bw::ReasonCode bits (state.h), append-only: unknown bit positions are kept
# as ``unknown_bit_<n>`` rather than dropped, so new codes stay visible.
REASON_NAMES = (
    "SustainedLowMotion",
    "ProlongedInactivity",
    "HrBelowBaseline",
    "HrTrendChange",
    "AbruptMotionDrop",
    "TimeOfDayTypicalDrowsy",
    "MatchesPastConfirmedEvent",
    "UserMarked",
    "PlannedNap",
    "LowSignalQuality",
    "SensorUnavailable",
    "DeviceNotWorn",
    "SustainedLowAlertnessProbability",
    "SustainedWakefulnessProbability",
    "InsufficientEvidence",
    "PossibleFall",
    "PostureChange",
    "ShortMotionBurst",
)


def _name(table: tuple[str, ...], value: int, kind: str) -> str:
    if value < len(table):
        return table[value]
    return f"unknown_{kind}_{value}"


def reason_names(reasons: int) -> list[str]:
    names = [name for bit, name in enumerate(REASON_NAMES) if reasons & (1 << bit)]
    # Bits past the known table stay visible instead of being dropped, so an
    # appended bw::ReasonCode shows up as data, not as silence.
    for bit in range(len(REASON_NAMES), 32):
        if reasons & (1 << bit):
            names.append(f"unknown_bit_{bit}")
    return names


def decode(data: bytes) -> dict[str, object]:
    """Decode one 48-byte Event. Raises ValueError on a size mismatch."""
    if len(data) != EVENT_SIZE:
        raise ValueError(f"Event record must be {EVENT_SIZE} bytes, got {len(data)}")
    (
        t_start_ms,
        seq,
        boot_id,
        duration_ms,
        t_start_unix,
        confidence,
        reasons,
        payload,
        quality_imu,
        quality_ppg,
        type_value,
        label,
        context,
        alertness,
        detector_id,
        schema_version,
    ) = _EVENT_STRUCT.unpack(data)
    return {
        "t_start_ms": t_start_ms,
        "seq": seq,
        "boot_id": boot_id,
        "duration_ms": duration_ms,
        "t_start_unix": t_start_unix,
        "confidence": confidence,
        "reasons": reasons,
        "reason_names": reason_names(reasons),
        "payload_hex": payload.hex(),
        "quality_imu": _name(QUALITY_NAMES, quality_imu, "quality"),
        "quality_ppg": _name(QUALITY_NAMES, quality_ppg, "quality"),
        "type": _name(EVENT_TYPE_NAMES, type_value, "type"),
        "label": _name(USER_LABEL_NAMES, label, "label"),
        "activity": _name(ACTIVITY_NAMES, context, "activity"),
        "alertness": _name(ALERTNESS_NAMES, alertness, "alertness"),
        "detector_id": detector_id,
        "schema_version": schema_version,
    }


def decode_all(blob: bytes) -> list[dict[str, object]]:
    """Decode a file of back-to-back 48-byte records."""
    if len(blob) % EVENT_SIZE != 0:
        raise ValueError(
            f"blob length {len(blob)} is not a multiple of {EVENT_SIZE} — truncated capture?"
        )
    return [decode(blob[i : i + EVENT_SIZE]) for i in range(0, len(blob), EVENT_SIZE)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Decode 48-byte bw::Event records.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--hex", dest="hex_text", help="one record as hex text")
    source.add_argument("file", nargs="?", help="binary file of back-to-back records")
    args = parser.parse_args(argv)

    try:
        if args.hex_text:
            records = [decode(bytes.fromhex(args.hex_text))]
        else:
            records = decode_all(Path(args.file).read_bytes())
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    for record in records:
        print(json.dumps(record))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
