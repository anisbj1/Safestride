"""Immutable value models shared across the companion layers (spec §7).

Contracts: `Event`/`StateSnapshot` (data_decision.md §4.5), `Command` (§4.6),
BLE_GATT.md §4 payloads, SYNC_PROTOCOL.md §3.2 STATUS. No I/O, no bleak, no sqlite.
"""

from __future__ import annotations

from dataclasses import dataclass

# [HW] Event schema version byte (data_decision.md §4.5 `Event::kSchemaVersion`).
# An unknown schema is stored raw and flagged unsupported — decoded fields for it are
# never rendered as data (D5 / spec §5 "never guessed").
EVENT_SCHEMA_VERSION = 1

# [HW] `EventType` index of TimeSync (data_decision.md §4.5, append-only enum): the
# event kind external.md §6.1 says the device records on SetTime; feeds wall-time
# reconstruction (D6, SYNC §4.5).
EVENT_TYPE_TIME_SYNC = 10


@dataclass(frozen=True)
class DeviceInfo:
    """Identity read once per connection (BLE_GATT §2/§3 DIS rows, §8 serial)."""

    device_id: str  # device BLE address as lowercase 12-hex (BLE_GATT §2)
    fw: str = ""
    hw: str = ""
    sw: str = ""


@dataclass(frozen=True)
class LiveStatus:
    """Decoded Live Status image, 14 B v1 (BLE_GATT §4.1). Enums stay ints:
    rendering decides known vs `unknown (#n)` (BLE_GATT §1)."""

    alertness: int
    wear: int
    activity: int
    quality: int
    mode: int
    power: int
    in_state_s: int
    confidence_pct: int  # heuristic confidence 0..100, NOT clinical certainty
    reasons: int  # 32-bit ReasonMask


@dataclass(frozen=True)
class Battery:
    """Battery Level (BLE_GATT §4.5): percent, or None for 0xFF = unknown
    (bad data is never a percentage)."""

    percent: int | None


@dataclass(frozen=True)
class EventRecord:
    """Canonical 48 B Event image decoded by contract offsets (SYNC §3.4,
    data_decision.md §4.5). `raw` is the authoritative bytes (D5); `wall_unix`
    follows SYNC §4.5 (0 = unknown, flagged in the UI)."""

    t_start_ms: int
    seq: int
    boot_id: int
    duration_ms: int
    t_start_unix: int
    confidence: float  # 0..1 model/heuristic confidence, NOT clinical certainty
    reasons: int
    payload: bytes  # 8 B
    quality: int  # SourceQuality: low byte imu, high byte ppg (bw/quality.h)
    type: int
    label: int
    context: int
    alertness_at_start: int
    detector_id: int
    schema_version: int
    raw: bytes  # 48 B, contract-authoritative (SYNC §3.4)
    wall_unix: int = 0  # 0 = unknown, flagged in UI (SYNC §4.5); never fabricated

    @property
    def supported(self) -> bool:
        """False for unknown schema versions: stored raw, rendered as unsupported."""
        return self.schema_version == EVENT_SCHEMA_VERSION


@dataclass(frozen=True)
class TimeSyncPoint:
    """One TimeSync event (external.md §6.1) for SYNC §4.5 reconstruction."""

    boot_id: int
    t_mono_ms: int
    unix_s: int


@dataclass(frozen=True)
class SyncStatus:
    """SYNC §3.2 STATUS frame view: last_seq/last_synced + the four flag bits.

    `linked` is False only for F21's NullSyncPort (D4) so app/CLI can report
    "sync client not linked" before F20's lib/sync/** half lands.
    """

    last_seq: int = 0
    last_synced: int = 0
    link_up: bool = False
    streaming: bool = False
    mtu_too_low: bool = False
    transfer_aborted: bool = False
    linked: bool = True
