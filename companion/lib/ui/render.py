"""Plain-text rendering of view-model state (D3: CLI-first, no TUI/GUI framework).

No logic: every line is assembled from fields the view-models already decided.
Wording rules (possible/suspected + confidence, ages, unknown flags) live in
domain/wording.py and are applied there, not here.
"""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.features.live import LiveViewState
from companion.features.sync_control import SyncViewState
from companion.features.timeline import TimelineRow
from companion.transport.ports import ScanResult


def render_live(state: LiveViewState) -> str:
    """Live status block; the not-live banner (§10) leads when disconnected."""
    lines: list[str] = []
    if state.banner:
        lines.append(f"!! {state.banner}")
    lines.extend(
        [
            f"state    : {state.state_text}",
            f"wear     : {state.wear_text}",
            f"activity : {state.activity_text}",
            f"battery  : {state.battery_text}",
            f"heart    : {state.hr_text}",
            f"sync     : {state.sync_text}",
        ]
    )
    return "\n".join(lines)


def render_timeline(rows: list[TimelineRow]) -> str:
    """Read-only timeline, newest first; unknown wall time is flagged inline (§10)."""
    if not rows:
        return "timeline: no synced events yet"
    lines = ["timeline (newest first):"]
    for row in rows:
        when = row.time_text if row.time_known else f"{row.time_text} (time unknown)"
        lines.append(f"  #{row.seq:<6} {when}  {row.label}  [{row.reasons_text}]")
    return "\n".join(lines)


def render_sync(state: SyncViewState) -> str:
    """Sync control summary: state + message, progress counters, raw STATUS flags."""
    status = state.status
    return "\n".join(
        [
            f"sync: {state.state} — {state.message}",
            f"progress: received {state.received} record(s), stored {state.stored} new, "
            f"acked seq {state.acked_seq}",
            render_sync_status(status),
        ]
    )


def render_sync_status(status: SyncStatus) -> str:
    """STATUS frame fields verbatim (SYNC §3.2) — flags are never dropped."""
    return (
        f"status: last_seq={status.last_seq} last_synced={status.last_synced} "
        f"link_up={status.link_up} streaming={status.streaming} "
        f"mtu_too_low={status.mtu_too_low} transfer_aborted={status.transfer_aborted} "
        f"linked={status.linked}"
    )


def render_status(
    *,
    device_id: str,
    firmware: str,
    battery_text: str,
    mtu: int,
    sync_status: SyncStatus,
) -> str:
    """`bench status` block: identity, battery, negotiated MTU, sync status."""
    return "\n".join(
        [
            f"device  : {device_id}",
            f"firmware: {firmware or 'unknown'}",
            f"battery : {battery_text}",
            f"ATT MTU : {mtu}",
            render_sync_status(sync_status),
        ]
    )


def render_connect(device_id: str, mtu: int) -> str:
    """`bench connect` confirmation including the negotiated ATT MTU (D8)."""
    return f"connected: {device_id}\nATT MTU : {mtu}"


def render_scan(results: list[ScanResult]) -> str:
    """Filtered scan results (service UUID + local name, BLE_GATT §2)."""
    if not results:
        return "no devices found (is the bracelet powered and in range?)"
    lines = [f"found {len(results)} device(s):"]
    for result in results:
        lines.append(f"  {result.address}  {result.name}")
    return "\n".join(lines)
