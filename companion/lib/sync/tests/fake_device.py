"""Scripted device side of SYNC_PROTOCOL.md §3–§4, over F21's `FakeTransport`.

Fully deterministic: no randomness and no wall clock — the harness injects time
(`now_ms`) and calls `pump()` each loop round. Like a real BLE stack, the device
**never reacts inside the phone's write call**: writes are recorded, and every
device→phone emission (STATUS, chunks, resends) happens on `pump()` — the same
asynchrony the NimBLE host task introduces on hardware.

Fault scripts are `set_fault(kind, nth)` on the nth Data-frame emission, and
`faults_fired` lets tests assert the injector actually ran (same discipline as
the firmware's fault matrix, F20 spec §9 P03). Acks are validated per §4.3
(`> marker` AND exists-in-store); the newest CURSOR is authoritative (§3.1).
"""

from __future__ import annotations

from collections.abc import Callable, Mapping

from companion.sync.frames import (
    STATUS_FLAG_LINK_UP,
    CursorFrame,
    decode_cursor,
    encode_chunk,
    encode_status,
)
from companion.transport.fake_transport import FakeTransport
from companion.transport.ports import SYNC_CONTROL_UUID, SYNC_DATA_UUID

from .sync_test_support import record_seq

# [EXPERIMENTAL] Device-side contract values the fake mirrors (SYNC §4.2):
# window of 4 unacked chunks, resend 5000 ms after the oldest unacked chunk.
DEVICE_MAX_UNACKED_CHUNKS = 4
DEVICE_ACK_TIMEOUT_MS = 5000


class FakeSyncDevice:
    """Serves `events` (seq -> raw 48 B) exactly per the frozen contract."""

    def __init__(
        self,
        transport: FakeDeviceTransport,
        events: Mapping[int, bytes],
        *,
        now_ms: Callable[[], int],
        mtu: int,
    ) -> None:
        self._transport = transport
        self._events: dict[int, bytes] = dict(events)
        self._now_ms = now_ms
        self._mtu = mtu
        self.head = max(events, default=0)
        self.marker = 0  # last_synced (§4.3): moves only on a valid ack
        self.cursors: list[CursorFrame] = []  # every CURSOR the phone ever wrote
        self.serving = True  # False = device stall (CT22): keeps request, sends nothing
        self.fault: tuple[str, int] | None = None  # (kind, nth emission)
        self.faults_fired = 0
        self.resends = 0
        self.frames_emitted = 0  # logical chunk sends (CT17: 24 events over 5 chunks)
        self.ignored_acks = 0
        self._emission_no = 0
        self._pending_status = False  # §3.2: answer on query / transfer complete
        self._chunks: list[tuple[int, list[bytes]]] = []  # (chunk_no, records)
        self._next_idx = 0
        self._in_flight: list[tuple[int, int]] = []  # (chunk idx, sent_ms)

    # --- harness controls -----------------------------------------------------

    def set_fault(self, kind: str, nth: int) -> None:
        """Script one fault on the nth Data-frame emission (drop/corrupt/duplicate)."""
        self.fault = (kind, nth)

    def add_events(self, events: Mapping[int, bytes]) -> None:
        """Grow the device store (CT18: new events after a first full run)."""
        self._events.update(events)
        self.head = max(self.head, max(events, default=0))

    # --- transport hook: record the write, react on the next pump() -----------

    def on_cursor(self, frame: bytes) -> None:
        """Sync Control write from the phone (§3.1): fields processed independently."""
        cursor = decode_cursor(frame)
        self.cursors.append(cursor)
        if cursor.is_status_query:
            self._pending_status = True
            return
        if cursor.ack_seq:
            self._apply_ack(cursor.ack_seq)
        if cursor.from_seq >= 1:
            self._start_transfer(cursor.from_seq, cursor.max_events)  # newest wins (§3.1)

    # --- §4.3 ack validation --------------------------------------------------

    def _apply_ack(self, ack_seq: int) -> None:
        if ack_seq > self.marker and self._events.get(ack_seq) is not None:
            self.marker = ack_seq  # exists-in-store check also bounds by head
        else:
            self.ignored_acks += 1
        self._in_flight = [
            (idx, sent_ms)
            for idx, sent_ms in self._in_flight
            if self._chunk_max_seq(idx) > ack_seq
        ]
        if self._next_idx >= len(self._chunks) and not self._in_flight:
            self._pending_status = True  # transfer complete (§3.2, V5 transcript)

    def _chunk_max_seq(self, idx: int) -> int:
        return max(record_seq(raw) for raw in self._chunks[idx][1])

    # --- §4.2 streaming + flow control ---------------------------------------

    def _records_per_chunk(self) -> int:
        usable = min(self._mtu - 3, 252)  # ATT MTU - 3, capped (SYNC §2)
        return max(1, (usable - 4) // 48)  # whole records only (§3.3)

    def _start_transfer(self, from_seq: int, max_events: int) -> None:
        seqs = sorted(s for s in self._events if s >= from_seq)
        if max_events:  # §3.1: per-request cap; 0 = no request-cap
            seqs = seqs[:max_events]
        per = self._records_per_chunk()
        self._chunks = []
        for i in range(0, len(seqs), per):
            batch = [self._events[s] for s in seqs[i : i + per]]
            self._chunks.append((len(self._chunks) + 1, batch))  # chunk_no restarts
        self._next_idx = 0
        self._in_flight = []  # new request aborts the old window (§3.1)

    def pump(self) -> None:
        """One harness round (the device task): STATUS answer, window fill, resend."""
        if not self.serving:
            return
        if self._pending_status:
            self._pending_status = False
            self._send_status()
        while (
            self._next_idx < len(self._chunks)
            and len(self._in_flight) < DEVICE_MAX_UNACKED_CHUNKS
        ):
            idx = self._next_idx
            self._next_idx += 1
            self._emit_chunk(idx)
        if self._in_flight:
            _idx, sent_ms = self._in_flight[0]
            if self._now_ms() - sent_ms >= DEVICE_ACK_TIMEOUT_MS:
                self.resends += 1
                now = self._now_ms()
                for idx, _sent_ms in list(self._in_flight):  # unacked suffix (§4.2)
                    chunk_no, records = self._chunks[idx]
                    self._send_data(encode_chunk(chunk_no, records))  # same chunk_no
                self._in_flight = [(idx, now) for idx, _sent_ms in self._in_flight]

    def _emit_chunk(self, idx: int) -> None:
        chunk_no, records = self._chunks[idx]
        frame = bytearray(encode_chunk(chunk_no, records))
        self._emission_no += 1
        kind, nth = self.fault if self.fault is not None else ("none", 0)
        fired = self._emission_no == nth
        if kind == "drop" and fired:
            # Only the FIRED emission vanishes in transit (no SEND); the device
            # still tracks it in flight below — it believes it sent, and the
            # §4.2 timeout resend (never faulted) heals the hole. All other
            # fresh emissions under a drop script send normally.
            self.faults_fired += 1
        else:
            if kind == "corrupt" and fired:
                self.faults_fired += 1
                frame[-1] ^= 0xFF  # flip a CRC byte (§4.7 corrupted-chunk row)
            self._send_data(bytes(frame))
            if kind == "duplicate" and fired:
                self.faults_fired += 1
                self._send_data(bytes(frame))  # byte-identical duplicate (V5)
        self._in_flight.append((idx, self._now_ms()))
        self.frames_emitted += 1

    # --- device → phone emissions (guarded: needs the peer's subscription) -----

    def _send_status(self) -> None:
        if SYNC_CONTROL_UUID not in self._transport.notify_cbs:
            return
        self._transport.emit(
            SYNC_CONTROL_UUID, encode_status(self.head, self.marker, STATUS_FLAG_LINK_UP)
        )

    def _send_data(self, frame: bytes) -> None:
        if SYNC_DATA_UUID not in self._transport.notify_cbs:
            return
        self._transport.emit(SYNC_DATA_UUID, frame)


class FakeDeviceTransport(FakeTransport):
    """F21's FakeTransport + one hook: Sync Control writes reach the device.

    With `auto_pump = True` the device also reacts immediately after processing
    the write (models a device that answers without waiting for the harness
    round — used by the runner tests, which have no pump of their own)."""

    device: FakeSyncDevice | None = None
    auto_pump: bool = False

    async def write(self, char: str, data: bytes) -> None:  # type: ignore[override]
        await super().write(char, data)
        if self.device is not None and char == SYNC_CONTROL_UUID:
            self.device.on_cursor(bytes(data))
            if self.auto_pump:
                self.device.pump()


def make_device_link(
    events: Mapping[int, bytes],
    *,
    now_ms: Callable[[], int],
    mtu: int = 255,
) -> tuple[FakeSyncDevice, FakeDeviceTransport]:
    """Wire a device and transport to each other (transport first, then device)."""
    transport = FakeDeviceTransport(mtu=mtu)
    device = FakeSyncDevice(transport, events, now_ms=now_ms, mtu=mtu)
    transport.device = device
    return device, transport
