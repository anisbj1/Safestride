"""SyncPort seam + Null/Fake implementations (spec §7, D4).

Split of duty (D4): **F20 owns** `companion/lib/sync/**` — framing, CRC, cursor,
ack and flow control of SYNC_PROTOCOL.md. **F21 owns** this port, the byte codecs,
validation, repository upsert, orchestration and UI. The record sink delivers raw
48 B slices (`on_records(list[bytes])`) so decode ownership stays wholly in F21.
Until F20's half lands, NullSyncPort degrades cleanly ("sync client not linked")
and FakeSyncPort drives host tests and scripted bench runs.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Protocol, runtime_checkable

from companion.data.models import SyncStatus

RecordSink = Callable[[list[bytes]], None]
StatusSink = Callable[[SyncStatus], None]


@runtime_checkable
class SyncPort(Protocol):
    """Seam frozen at PLAN (spec §7): cursor requests, cumulative ack, status,
    and two sinks. Async to match bleak/F20's client natively; tests drive it with
    `asyncio.run(...)` — no pytest-asyncio dependency."""

    async def request_from(self, from_seq: int, max_events: int = 0) -> None:
        """CURSOR: start/restart streaming at from_seq (SYNC §3.1); max_events 0 = uncapped."""

    async def ack(self, seq: int) -> None:
        """Cumulative ack: highest seq durably stored by the companion (SYNC §4.3)."""

    def status(self) -> SyncStatus:
        """Latest device STATUS view (SYNC §3.2), incl. the linked=False Null case."""

    def set_record_sink(self, cb: RecordSink | None) -> None:
        """Callback receiving lists of exactly-48-B raw records (SYNC §3.3 payload)."""

    def set_status_sink(self, cb: StatusSink | None) -> None:
        """Callback receiving each new SyncStatus (link-up, STATUS frames, aborts)."""


class NullSyncPort:
    """'Sync client not linked' (D4): F20's lib/sync/** implements SyncPort in the
    same wave; until then app/CLI/report timeline and timesync work from the local
    DB and `sync` reports not-linked without crashing (spec §10)."""

    def __init__(self) -> None:
        self._record_sink: RecordSink | None = None
        self._status_sink: StatusSink | None = None

    async def request_from(self, from_seq: int, max_events: int = 0) -> None:
        return None  # no client behind this port: nothing is requested, nothing fails

    async def ack(self, seq: int) -> None:
        return None

    def status(self) -> SyncStatus:
        return SyncStatus(linked=False)

    def set_record_sink(self, cb: RecordSink | None) -> None:
        self._record_sink = cb

    def set_status_sink(self, cb: StatusSink | None) -> None:
        self._status_sink = cb


class FakeSyncPort:
    """Scripted double (D4): records requests/acks, delivers canned records/status,
    never talks to hardware. Tests and the CLI's scripted runs drive it directly."""

    def __init__(self, *, status: SyncStatus | None = None) -> None:
        self.requests: list[tuple[int, int]] = []
        self.acks: list[int] = []
        self._status = status or SyncStatus(linked=True, link_up=True)
        self._record_sink: RecordSink | None = None
        self._status_sink: StatusSink | None = None

    async def request_from(self, from_seq: int, max_events: int = 0) -> None:
        self.requests.append((from_seq, max_events))

    async def ack(self, seq: int) -> None:
        self.acks.append(seq)
        self._status = SyncStatus(
            last_seq=max(self._status.last_seq, seq),
            last_synced=max(self._status.last_synced, seq),
            link_up=self._status.link_up,
            streaming=self._status.streaming,
            mtu_too_low=self._status.mtu_too_low,
            transfer_aborted=self._status.transfer_aborted,
            linked=self._status.linked,
        )

    def status(self) -> SyncStatus:
        return self._status

    def set_record_sink(self, cb: RecordSink | None) -> None:
        self._record_sink = cb

    def set_status_sink(self, cb: StatusSink | None) -> None:
        self._status_sink = cb

    # --- test/scripting surface ---------------------------------------------

    def emit_records(self, records: list[bytes]) -> None:
        """Simulate received chunk payloads (already split into raw 48 B records)."""
        if self._record_sink is not None:
            self._record_sink(list(records))

    def emit_status(self, status: SyncStatus) -> None:
        """Simulate a STATUS indication (SYNC §3.2), including flag-bit changes."""
        self._status = status
        if self._status_sink is not None:
            self._status_sink(status)
