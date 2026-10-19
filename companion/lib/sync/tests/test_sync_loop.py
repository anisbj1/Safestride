"""CT17–CT23: floor — full fake-device loop over F21's FakeTransport + real
`EventRepository` (tmp SQLite) + `SyncControlViewModel`, driven by `run()`
with a FakeClock (spec §9). Every round asserts the P04 mirror: the device
marker and every phone cursor are never ahead of the committed rows.
"""

from __future__ import annotations

import pathlib

import pytest
from companion.data import db
from companion.data.event_repository import EventRepository
from companion.features import sync_control
from companion.features.sync_control import SyncControlViewModel
from companion.sync.client import SyncClient

from .fake_device import make_device_link
from .sync_test_support import FakeClock, make_event48, run

# [DERIVED] CT17 happy path: 24 events pack into exactly 5 chunks at ATT MTU 255
# (5 records/chunk, SYNC §3.3; window 4 per §4.2).
EVENT_COUNT = 24
# [EXPERIMENTAL] Harness round = 100 ms of simulated time (no wall clock/sleeps).
STEP_MS = 100
# [EXPERIMENTAL] 600 rounds = 60 s simulated — covers two full 5 s stall cycles.
MAX_ROUNDS = 600


def _source_events(count: int = EVENT_COUNT, *, start: int = 1) -> dict[int, bytes]:
    return {seq: make_event48(seq) for seq in range(start, start + count)}


class Harness:
    """One companion (client + view-model + DB) wired to one scripted device."""

    def __init__(self, tmp_path: pathlib.Path, events: dict[int, bytes], *, mtu: int = 255):
        self.clock = FakeClock()
        self.device, self.transport = make_device_link(events, now_ms=self.clock.now_ms, mtu=mtu)
        run(self.transport.connect(None))  # F21's fake guards every call on `connected`
        self.conn = db.open_db(tmp_path / "sync.sqlite3")
        self.repo = EventRepository(self.conn)
        self.device_id = self.transport.device_info.device_id
        self.mtu = mtu
        self.client: SyncClient
        self.vm: SyncControlViewModel
        self._bind()

    def _bind(self) -> None:
        """Fresh client + view-model over the same DB — companion reboot (§4.6)."""
        self.client = SyncClient(self.transport)
        self.vm = SyncControlViewModel(
            self.repo,
            self.client,
            device_id=self.device_id,
            now_ms=self.clock.now_ms,
            mtu_size=self.mtu,
        )
        self.client.set_record_sink(self.vm.on_records)

    def reboot_companion(self) -> None:
        self._bind()

    def check_invariants(self) -> None:
        """P04 mirror + cursor-never-ahead (spec §9 CT17–CT23), every round."""
        hw = self.repo.high_water(self.device_id)
        assert self.device.marker <= hw, "device marker ahead of committed rows"
        for cursor in self.device.cursors:
            assert cursor.from_seq - 1 <= hw, f"cursor ahead of committed rows: {cursor}"

    def stored_seqs(self) -> list[int]:
        rows = self.repo.timeline(EVENT_COUNT + 8, device_id=self.device_id)
        return sorted(row.seq for row in rows)


async def drive(h: Harness):
    """start() + bounded tick loop with device pump and per-round invariants."""
    view = await h.vm.start()
    h.check_invariants()
    rounds = 0
    while (
        view.state in (sync_control.STATE_RUNNING, sync_control.STATE_STALLED)
        and rounds < MAX_ROUNDS
    ):
        h.clock.advance(STEP_MS)
        h.device.pump()
        view = await h.vm.tick()
        h.check_invariants()
        rounds += 1
    return view


def _assert_complete(h: Harness, view, *, expected: int = EVENT_COUNT) -> None:
    assert view.state == sync_control.STATE_DONE
    assert h.repo.event_count(h.device_id) == expected
    assert h.repo.high_water(h.device_id) == expected
    assert h.device.marker == expected


def test_CT17_happy_path_24_events_five_chunks(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    view = run(drive(h))
    _assert_complete(h, view)
    assert h.device.frames_emitted == 5  # 24 events over 5 chunks at MTU 255
    assert (view.received, view.stored) == (EVENT_COUNT, EVENT_COUNT)
    assert h.client.duplicate_records == 0
    assert h.device.ignored_acks == 0 and h.device.faults_fired == 0
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))


def test_CT18_idempotent_resume_and_companion_reboot(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    view1 = run(drive(h))
    _assert_complete(h, view1)
    emitted_after_first_run = h.device.frames_emitted
    # Second full run (fresh client + view-model, same DB): transfers nothing.
    h.reboot_companion()
    view2 = run(drive(h))
    assert view2.state == sync_control.STATE_DONE
    assert view2.received == 0 and h.client.records_delivered == 0
    assert h.repo.event_count(h.device_id) == EVENT_COUNT  # zero dupes
    assert h.device.frames_emitted == emitted_after_first_run  # nothing re-sent
    # Companion reboot with new device events: resume from hw+1, converge.
    h.device.add_events(_source_events(6, start=25))
    h.reboot_companion()
    view3 = run(drive(h))
    _assert_complete(h, view3, expected=30)
    assert h.stored_seqs() == list(range(1, 31))


def test_CT19_dropped_chunk_no_ack_past_hole_resend_heals(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    h.device.set_fault("drop", 3)
    view = run(drive(h))
    assert h.device.faults_fired == 1
    assert h.device.resends >= 1  # §4.2 timeout resend filled the hole
    assert h.client.frames_dropped == 0  # nothing invalid ever reached the sink
    _assert_complete(h, view)
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))


def test_CT20_corrupted_chunk_crc_dropped_resend_heals(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    h.device.set_fault("corrupt", 2)
    view = run(drive(h))
    assert h.device.faults_fired == 1
    assert h.client.frames_dropped == 1  # CRC reject at the phone (§4.7)
    assert h.device.resends >= 1
    _assert_complete(h, view)
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))


def test_CT21_duplicated_chunk_no_dup_rows_marker_advances_once(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    h.device.set_fault("duplicate", 1)  # chunk 1 = seq 1..5 delivered twice
    view = run(drive(h))
    assert h.device.faults_fired == 1
    assert h.client.duplicate_records == 5  # the duplicate was seen and dropped
    assert view.received == EVENT_COUNT  # the sink saw each record exactly once
    assert h.device.ignored_acks == 0  # no second ack was ever produced (D10)
    _assert_complete(h, view)
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))


def test_CT22_device_stall_then_serves_the_stall_re_request(tmp_path) -> None:
    h = Harness(tmp_path, _source_events())
    h.device.serving = False  # device stops serving (companion-side F04/F08 analog)

    async def scenario():
        view = await h.vm.start()
        h.check_invariants()
        saw_stalled = False
        rounds = 0
        while view.state != sync_control.STATE_DONE and rounds < MAX_ROUNDS:
            h.clock.advance(STEP_MS)
            if saw_stalled:
                h.device.serving = True  # device comes back after the phone noticed
            h.device.pump()
            view = await h.vm.tick()
            if view.state == sync_control.STATE_STALLED:
                saw_stalled = True
            h.check_invariants()
            rounds += 1
        return view, saw_stalled

    view, saw_stalled = run(scenario())
    assert saw_stalled, "the view must surface a stall at least once"
    _assert_complete(h, view)
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))


_MATRIX = (
    [("drop", nth) for nth in range(1, 6)]
    + [("corrupt", nth) for nth in range(1, 6)]
    + [("duplicate", nth) for nth in range(1, 6)]
)


@pytest.mark.parametrize("kind,nth", _MATRIX, ids=[f"{kind}@{nth}" for kind, nth in _MATRIX])
def test_CT23_lossy_matrix_zero_loss_zero_dupes(tmp_path, kind: str, nth: int) -> None:
    h = Harness(tmp_path, _source_events())
    h.device.set_fault(kind, nth)
    view = run(drive(h))
    assert h.device.faults_fired == 1  # the injector actually fired
    _assert_complete(h, view)  # marker == head; invariants held every round
    assert h.stored_seqs() == list(range(1, EVENT_COUNT + 1))  # set == source set
