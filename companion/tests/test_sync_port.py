"""§9 test 7 — SyncPort seam (D4): NullSyncPort degrades cleanly before F20's half
lands; FakeSyncPort drives a stall scenario (no progress for kSyncStallTimeoutMs →
"stalled" + re-request) and duplicate records upsert as no-ops (SYNC §4.4)."""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.features import sync_control
from companion.features.sync_control import (
    SYNC_REQUEST_BATCH,
    SYNC_STALL_TIMEOUT_MS,
    SyncControlViewModel,
)
from companion.sync_port import FakeSyncPort, NullSyncPort
from conftest import run
from vectors import make_event_bytes


def test_null_port_reports_not_linked_and_app_degrades_cleanly(repo, clock):
    port = NullSyncPort()
    vm = SyncControlViewModel(
        repo, port, device_id="dev1", now_ms=clock.now_ms, mtu_size=185
    )
    view = run(vm.start())
    assert view.state == sync_control.STATE_NOT_LINKED
    assert "sync client not linked" in view.message
    assert view.status.linked is False
    run(vm.tick())  # ticking a not-linked VM never crashes (spec §10)


def test_null_port_status_and_sinks_are_safe():
    port = NullSyncPort()
    assert port.status().linked is False
    port.set_record_sink(lambda records: None)
    port.set_status_sink(lambda status: None)
    assert run(port.request_from(1)) is None
    assert run(port.ack(1)) is None


def test_fake_port_stall_re_requests_after_the_timeout(repo, clock):
    device = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=100))
    vm = SyncControlViewModel(
        repo, device, device_id="dev1", now_ms=clock.now_ms, mtu_size=185
    )
    run(vm.start())
    assert device.requests == [(1, SYNC_REQUEST_BATCH)]  # from_seq = hw + 1
    run(vm.tick())  # within the stall window: still running
    clock.advance(SYNC_STALL_TIMEOUT_MS)
    view = run(vm.tick())
    assert view.state == sync_control.STATE_STALLED
    assert "stalled" in view.message
    assert device.requests == [(1, SYNC_REQUEST_BATCH), (1, SYNC_REQUEST_BATCH)]


def test_progress_after_a_stall_clears_the_stalled_state(repo, clock):
    device = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=100))
    vm = SyncControlViewModel(
        repo, device, device_id="dev1", now_ms=clock.now_ms, mtu_size=185
    )
    device.set_record_sink(vm.on_records)
    run(vm.start())
    clock.advance(SYNC_STALL_TIMEOUT_MS)
    run(vm.tick())
    device.emit_records([make_event_bytes(seq=1)])
    view = vm.view()
    assert view.state == sync_control.STATE_RUNNING  # progress ended the stall


def test_duplicate_records_upsert_as_no_ops_and_ack_converges(repo, clock):
    device = FakeSyncPort()
    vm = SyncControlViewModel(
        repo, device, device_id="dev1", now_ms=clock.now_ms, mtu_size=185
    )
    device.set_record_sink(vm.on_records)
    run(vm.start())
    records = [make_event_bytes(seq=1), make_event_bytes(seq=2)]
    device.emit_records(records)
    device.emit_records(records)  # byte-identical retransmission (SYNC §6 V5)
    view = run(vm.tick())
    assert view.received == 4
    assert view.stored == 2  # duplicates were no-ops
    assert repo.event_count("dev1") == 2
    assert device.acks == [2]  # cumulative ack moved exactly once


def test_status_sink_receives_flag_updates(repo, clock):
    device = FakeSyncPort()
    seen: list[SyncStatus] = []
    device.set_status_sink(seen.append)
    device.emit_status(SyncStatus(linked=True, link_up=True, streaming=True))
    assert [s.streaming for s in seen] == [True]
