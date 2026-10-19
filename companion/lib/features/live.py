"""Live status view-model: device state, battery, HR, link/sync status (spec §7).

Reads flow in as raw bytes (`on_*`) so the model is transport-agnostic and fully
host-testable with FakeTransport scripts (D2). Cached values are never cleared on
disconnect: they are shown **with their age**, and a "not live" banner replaces any
implied liveness (spec §5/§10).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from companion.data.models import LiveStatus, SyncStatus
from companion.domain import wording
from companion.sync_port import SyncPort
from companion.transport.codec import decode_battery, decode_hr, decode_live_status

# [HW] Device emits Live Status at most once per second while subscribed
# (BLE_GATT §4.1) → this is the UI refresh cadence (spec §8).
LIVE_STATUS_INTERVAL_MS = 1000
# [HW] Device caps HR notifications at 1 Hz (BLE_GATT §4.2 kHrNotifyIntervalMs);
# beyond that age the HR sample is shown as stale (spec §8).
HR_NOTIFY_INTERVAL_MS = 1000


@dataclass(frozen=True)
class LiveViewState:
    """Render-ready live status (ui/render.py draws it; no logic there)."""

    connected: bool
    banner: str  # "" while live; "not live ..." when disconnected (spec §10)
    state_text: str
    wear_text: str
    activity_text: str
    battery_text: str
    hr_text: str
    sync_text: str
    stale: bool  # newest cached value is older than the stale threshold


class LiveViewModel:
    def __init__(self, *, now_ms: Callable[[], int], sync_port: SyncPort | None = None) -> None:
        self._now_ms = now_ms
        self._port = sync_port
        self._connected = False
        self._live: tuple[LiveStatus, int] | None = None  # (value, received-at ms)
        self._battery_percent: tuple[int | None, int] | None = None
        self._hr_bpm: tuple[int, int] | None = None

    # --- inputs --------------------------------------------------------------

    def set_connected(self, connected: bool) -> None:
        self._connected = connected  # values stay: they are shown with age (§10)

    def on_live_status(self, raw: bytes, now_ms: int | None = None) -> None:
        """0 bytes = "no snapshot yet" (BLE_GATT §5) → keep whatever we had."""
        status = decode_live_status(raw)
        if status is None:
            return
        self._live = (status, now_ms if now_ms is not None else self._now_ms())

    def on_battery(self, raw: bytes, now_ms: int | None = None) -> None:
        battery = decode_battery(raw)
        self._battery_percent = (battery.percent, now_ms if now_ms is not None else self._now_ms())

    def on_hr(self, raw: bytes, now_ms: int | None = None) -> None:
        self._hr_bpm = (decode_hr(raw), now_ms if now_ms is not None else self._now_ms())

    # --- output --------------------------------------------------------------

    def snapshot(self) -> LiveViewState:
        """Values + their ages; never implies live data while disconnected (§10)."""
        now = self._now_ms()
        if self._live is not None:
            status, ts = self._live
            state_text = wording.format_enum(wording.ALERTNESS_NAMES, status.alertness)
            wear_text = wording.format_enum(wording.WEAR_NAMES, status.wear)
            activity_text = wording.format_enum(wording.ACTIVITY_NAMES, status.activity)
            fresh_ms = now - ts
        else:
            state_text = wear_text = activity_text = "no snapshot yet"
            fresh_ms = None
        battery = self._battery_text(now)
        hr = self._hr_text(now)
        if not self._connected:
            newest = self._last_ts()
            age = wording.age_text(now - newest) if newest is not None else "never"
            banner = f"not live — showing last known values, newest from {age}"
        else:
            banner = ""
        return LiveViewState(
            connected=self._connected,
            banner=banner,
            state_text=state_text,
            wear_text=wear_text,
            activity_text=activity_text,
            battery_text=battery,
            hr_text=hr,
            sync_text=self._sync_text(),
            stale=fresh_ms is not None and fresh_ms > wording.STALE_LABEL_MS,
        )

    # --- internals -----------------------------------------------------------

    def _last_ts(self) -> int | None:
        """Newest receive timestamp across all cached values (for the disconnect age)."""
        stamps: list[int] = []
        if self._live is not None:
            stamps.append(self._live[1])
        if self._battery_percent is not None:
            stamps.append(self._battery_percent[1])
        if self._hr_bpm is not None:
            stamps.append(self._hr_bpm[1])
        return max(stamps) if stamps else None

    def _battery_text(self, now: int) -> str:
        if self._battery_percent is None:
            return "no data yet"
        percent, ts = self._battery_percent
        value = wording.battery_text(percent)  # 0xFF → "unknown", never a percentage
        return f"{value} ({wording.age_text(now - ts)})"

    def _hr_text(self, now: int) -> str:
        if self._hr_bpm is None:
            return "no data yet"
        bpm, ts = self._hr_bpm
        # HR staleness uses the 1 Hz device cap (spec §8): silence ≠ a fresh reading.
        return f"{bpm} bpm ({wording.age_text(now - ts, stale_after_ms=HR_NOTIFY_INTERVAL_MS)})"

    def _sync_text(self) -> str:
        if self._port is None:
            return "no sync client"
        return format_sync_status(self._port.status())


def format_sync_status(status: SyncStatus) -> str:
    """One-line SyncStatus text shared by live view, status command and tests."""
    if not status.linked:
        return "sync client not linked"
    parts = ["link up" if status.link_up else "link down"]
    if status.streaming:
        parts.append("streaming")
    if status.mtu_too_low:
        parts.append("mtu_too_low")
    if status.transfer_aborted:
        parts.append("transfer_aborted")
    parts.append(f"last_seq={status.last_seq}")
    parts.append(f"last_synced={status.last_synced}")
    return ", ".join(parts)
