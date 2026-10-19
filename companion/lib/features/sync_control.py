"""Sync control view-model (spec §7): start from high_water+1, progress counters,
STATUS flags, stall detection with re-request, MTU fail-closed gate (D8).

Orchestration only — framing, CRC and cursor/ack transport belong to F20's client
behind SyncPort (D4). Driven by `start()` then `tick()` (CLI loop or tests via
`asyncio.run(...)`; no pytest-asyncio dependency).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from companion.data.event_repository import EventRepository
from companion.data.models import SyncStatus
from companion.sync_port import SyncPort
from companion.transport.codec import decode_event48
from companion.transport.ports import MIN_SYNC_MTU_BYTES

# [EXPERIMENTAL] CURSOR `max` per round (u16-bounded, SYNC §3.1): caps one transfer
# round on the M4 bench; tune with bench experience (spec §8).
SYNC_REQUEST_BATCH = 128
# [DERIVED] Companion stall timeout (spec §8): mirrors the device's kAckTimeoutMs
# = 5000 ms (SYNC §4.2) — no progress by then ⇒ show stalled and re-request; the
# phone drives recovery (SYNC §4.1), the device never depends on it.
SYNC_STALL_TIMEOUT_MS = 5000

# View-model states (plain strings: rendered directly, asserted in tests).
STATE_IDLE = "idle"
STATE_RUNNING = "running"
STATE_STALLED = "stalled"
STATE_DONE = "done"
STATE_NOT_LINKED = "not linked"
STATE_MTU_TOO_LOW = "mtu_too_low"


@dataclass(frozen=True)
class SyncViewState:
    """Render-ready sync control state (ui/render.py draws it)."""

    state: str
    message: str
    requested_from: int
    received: int  # raw records delivered by the port
    stored: int  # newly inserted rows (duplicates were no-ops, SYNC §4.4)
    acked_seq: int
    status: SyncStatus  # latest device STATUS frame (SYNC §3.2), flags included


class SyncControlViewModel:
    def __init__(
        self,
        repository: EventRepository,
        port: SyncPort,
        *,
        device_id: str,
        now_ms: Callable[[], int],
        mtu_size: int = 0,
        request_batch: int = SYNC_REQUEST_BATCH,
        stall_ms: int = SYNC_STALL_TIMEOUT_MS,
    ) -> None:
        self._repository = repository
        self._port = port
        self._device_id = device_id
        self._now_ms = now_ms
        self._mtu = mtu_size
        self._batch = request_batch
        self._stall_ms = stall_ms
        self._state = STATE_IDLE
        self._message = "not started"
        self._requested_from = 0
        self._received = 0
        self._stored = 0
        self._acked_seq = 0
        self._pending_ack: int | None = None
        self._last_progress = 0
        self._last_status = SyncStatus()

    def set_mtu(self, mtu_size: int) -> None:
        """Record the negotiated ATT MTU (D8: verified by the transport, never assumed)."""
        self._mtu = mtu_size

    async def start(self) -> SyncViewState:
        """Begin a sync round: `from_seq = high_water + 1` (SYNC §4.1). Fails closed
        below ATT MTU 55 *before any CURSOR write* (D8, backlog 37, spec §10)."""
        self._last_status = self._port.status()
        if not self._last_status.linked:
            self._state = STATE_NOT_LINKED
            self._message = "sync client not linked"
            return self.view()
        if self._mtu < MIN_SYNC_MTU_BYTES:
            self._state = STATE_MTU_TOO_LOW
            self._message = (
                f"mtu_too_low: ATT MTU {self._mtu} < {MIN_SYNC_MTU_BYTES} — sync refused"
            )
            return self.view()
        from_seq = self._repository.high_water(self._device_id) + 1
        await self._port.request_from(from_seq, self._batch)
        self._requested_from = from_seq
        self._state = STATE_RUNNING
        self._last_progress = self._now_ms()
        self._message = f"requested from seq {from_seq} (max {self._batch})"
        return self.view()

    def on_records(self, records: list[bytes]) -> None:
        """SyncPort record sink: upsert raw 48 B records, count progress, queue the
        cumulative ack (issued on the next tick)."""
        if not records:
            return
        self._received += len(records)
        self._stored += self._repository.upsert(self._device_id, records)
        highest = max(decode_event48(raw).seq for raw in records)
        if self._pending_ack is None:
            self._pending_ack = highest
        else:
            self._pending_ack = max(self._pending_ack, highest)
        self._last_progress = self._now_ms()
        if self._state in (STATE_RUNNING, STATE_STALLED):
            self._state = STATE_RUNNING  # progress ends a stall (SYNC §4.7 recovery)

    async def tick(self) -> SyncViewState:
        """One pump: apply the pending ack, then detect completion or a stall."""
        if self._pending_ack is not None:
            await self._port.ack(self._pending_ack)
            self._acked_seq = self._pending_ack
            self._pending_ack = None
        # Fetch status AFTER the ack: the device moves its marker on ack (SYNC §4.3),
        # so the STATUS read here reflects the committed state.
        self._last_status = self._port.status()
        if self._state == STATE_RUNNING:
            await self._check_progress()
        return self.view()

    async def _check_progress(self) -> None:
        status = self._last_status
        high_water = self._repository.high_water(self._device_id)
        if status.link_up and status.last_seq <= high_water:
            self._state = STATE_DONE
            self._message = f"done: device head seq {status.last_seq} <= stored {high_water}"
        elif self._now_ms() - self._last_progress >= self._stall_ms:
            # No progress for the stall window: show stalled and re-request from
            # hw+1 (phone-driven recovery, SYNC §4.1/§4.7; never the device's job).
            from_seq = high_water + 1
            await self._port.request_from(from_seq, self._batch)
            self._state = STATE_STALLED
            self._message = (
                f"stalled: no progress for {self._stall_ms} ms — re-requested from seq {from_seq}"
            )
            self._last_progress = self._now_ms()

    def view(self) -> SyncViewState:
        """Render-ready state; device STATUS flags are surfaced, never swallowed."""
        message = self._message
        flags = []
        if self._last_status.mtu_too_low:
            flags.append("mtu_too_low (device status)")
        if self._last_status.transfer_aborted:
            flags.append("transfer_aborted (device status)")
        if flags:
            message = f"{message} — {', '.join(flags)}"
        return SyncViewState(
            state=self._state,
            message=message,
            requested_from=self._requested_from,
            received=self._received,
            stored=self._stored,
            acked_seq=self._acked_seq,
            status=self._last_status,
        )
