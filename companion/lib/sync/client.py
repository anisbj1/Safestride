"""SyncClient — the real implementation of F21's `SyncPort` seam (D2/D9/D10/D14).

Pull-free, callback-driven client: it writes CURSOR frames on API calls and parses
STATUS/chunk frames arriving through F21's `GattTransport` notify callbacks. No
timers, no background task, no threads (D11): recovery is exactly one loop per
side — the device resends on ack timeout (SYNC §4.2), the view-model re-requests
on stall (`features/sync_control`), a link-up re-request is the next `start()`.

D9 contiguous gate: records are delivered to the sink only as the contiguous run
`base+1…`; `base` advances **after** the sink returns (committed), so F21's
`MAX(seq)` cursor (`data/event_repository.high_water`) never jumps a hole and the
device marker can never pass uncommitted rows. D10: the wire ack value is derived
from `base`, never from the caller's argument.
"""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.sync.frames import (
    EVENT_RECORD_BYTES,
    MAX_HELD_RECORDS,
    decode_chunk,
    decode_status,
    encode_cursor,
)
from companion.transport.codec import CodecError, decode_event48
from companion.transport.ports import MIN_SYNC_MTU_BYTES, SYNC_CONTROL_UUID, SYNC_DATA_UUID

__all__ = ["SyncClient", "SyncMtuError"]


class SyncMtuError(RuntimeError):
    """`transport.mtu_size < MIN_SYNC_MTU_BYTES` (SYNC §2): refuse before any
    write — defense in depth behind the view-model's D8 fail-closed gate."""


class SyncClient:
    """SyncPort over a live `GattTransport` (spec §7, D9/D10/D11/D14)."""

    def __init__(self, transport) -> None:
        self._transport = transport
        self._record_sink = None
        self._status_sink = None
        self._subscribed = False
        # D9 contiguity gate: base = highest seq committed through the sink.
        self._base = 0
        self._held: dict[int, bytes] = {}
        self._last_sent_ack = 0
        # linked=True from construction (CT15): this port *is* a real client;
        # link_up etc. only become true when the device's STATUS says so.
        self._status = SyncStatus(linked=True)
        # Diagnostics counters (spec §9/§10: dropped + counted, never a crash).
        self.frames_dropped = 0
        self.duplicate_records = 0
        self.held_dropped = 0
        self.sink_errors = 0
        self.acks_suppressed = 0
        self.records_delivered = 0
        self.chunks_received = 0

    # --- SyncPort seam (F21 `sync_port.py`) ----------------------------------

    async def request_from(self, from_seq: int, max_events: int = 0) -> None:
        """CURSOR start/restart at from_seq (SYNC §3.1). Also re-anchors `base`
        at the committed prefix implied by the view-model (`from_seq - 1 = hw`)."""
        self._require_mtu()
        await self._ensure_subscribed()
        self._base = max(self._base, from_seq - 1)  # base never decreases
        if self._held:
            self._held = {s: raw for s, raw in self._held.items() if s > self._base}
        await self._write(encode_cursor(from_seq, max_events))

    async def ack(self, seq: int) -> None:
        """Cumulative ack trigger (SYNC §4.3). The wire value is clamped to `base`
        by construction (D10): nothing beyond the committed prefix is ever acked,
        and an unchanged base is a suppressed re-ack (counted)."""
        self._require_mtu()
        await self._ensure_subscribed()
        del seq  # trigger only — the committed prefix is the authoritative value
        if self._base <= self._last_sent_ack:
            self.acks_suppressed += 1
            return
        await self._write(encode_cursor(0, 0, self._base))
        self._last_sent_ack = self._base

    def status(self) -> SyncStatus:
        """Latest device STATUS view (SYNC §3.2); linked=True until told otherwise."""
        return self._status

    def set_record_sink(self, cb) -> None:
        self._record_sink = cb

    def set_status_sink(self, cb) -> None:
        self._status_sink = cb

    # --- internals ------------------------------------------------------------

    def _require_mtu(self) -> None:
        mtu = self._transport.mtu_size
        if mtu < MIN_SYNC_MTU_BYTES:
            raise SyncMtuError(
                f"ATT MTU {mtu} < {MIN_SYNC_MTU_BYTES} — sync refused (no CURSOR written)"
            )

    async def _ensure_subscribed(self) -> None:
        """D14: subscribe lazily + idempotently, then immediately write one
        all-zero-body status-query CURSOR so `status()` reflects the device
        before the first transfer completes."""
        if self._subscribed:
            return
        await self._transport.start_notify(SYNC_CONTROL_UUID, self._on_control)
        await self._transport.start_notify(SYNC_DATA_UUID, self._on_data)
        self._subscribed = True
        await self._write(encode_cursor(0, 0, 0))  # §3.1 status query

    async def _write(self, frame: bytes) -> None:
        await self._transport.write(SYNC_CONTROL_UUID, frame)

    def _on_control(self, data: bytes) -> None:
        """Sync Control indication (STATUS, §3.2). Drop + count on §4.7 errors."""
        try:
            frame = decode_status(bytes(data))
        except Exception:
            self.frames_dropped += 1
            return
        self._status = SyncStatus(
            last_seq=frame.last_seq,
            last_synced=frame.last_synced,
            link_up=frame.flags.link_up,
            streaming=frame.flags.streaming,
            mtu_too_low=frame.flags.mtu_too_low,
            transfer_aborted=frame.flags.transfer_aborted,
            linked=True,
        )
        if self._status_sink is not None:
            self._status_sink(self._status)

    def _on_data(self, data: bytes) -> None:
        """Sync Data notification (chunk, §3.3): drop + count on §4.7 errors."""
        try:
            chunk = decode_chunk(bytes(data))
        except Exception:
            self.frames_dropped += 1
            return
        self.chunks_received += 1
        self._ingest(chunk.records)

    # --- D9 contiguity gate ---------------------------------------------------

    def _ingest(self, records: tuple[bytes, ...]) -> None:
        for raw in records:
            if len(raw) != EVENT_RECORD_BYTES:
                self.frames_dropped += 1
                continue
            try:
                seq = decode_event48(raw).seq
            except CodecError:
                self.frames_dropped += 1
                continue
            if seq <= self._base or seq in self._held:
                self.duplicate_records += 1
            else:
                self._held[seq] = raw
        self._release_contiguous()
        # Overflow (spec §10): drop held + count — safe because nothing beyond
        # `base` is ever acked, so the device retransmits the dropped records.
        if len(self._held) > MAX_HELD_RECORDS:
            self.held_dropped += len(self._held)
            self._held.clear()

    def _release_contiguous(self) -> None:
        """Deliver each contiguous run `base+1…` to the sink, in order (D9)."""
        while self._base + 1 in self._held:
            run: list[tuple[int, bytes]] = []
            seq = self._base + 1
            while seq in self._held:
                run.append((seq, self._held.pop(seq)))
                seq += 1
            if not self._deliver(run):
                return

    def _deliver(self, run: list[tuple[int, bytes]]) -> bool:
        """One sink call per contiguous run. `base` advances only **after** the
        sink returns (committed): a sink exception leaves `base` untouched, so
        no ack can pass uncommitted rows (P04 mirror, spec §10)."""
        if self._record_sink is None:
            for seq, raw in run:  # no sink yet: put back and wait
                self._held.setdefault(seq, raw)
            return False
        try:
            self._record_sink([raw for _, raw in run])
        except Exception:
            self.sink_errors += 1
            return False  # run already removed from held: device resends from base+1
        self._base = run[-1][0]
        self.records_delivered += len(run)
        return True
