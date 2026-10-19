"""§9 test 10 — sync control view-model: starts from high_water+1, progress
counters, STATUS flags (streaming / mtu_too_low / transfer_aborted) surfaced,
never crashes; fail-closed MTU gate writes no CURSOR (D8, spec §10)."""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.features import sync_control
from companion.features.sync_control import SYNC_REQUEST_BATCH, SyncControlViewModel
from companion.sync_port import FakeSyncPort
from conftest import run
from vectors import make_event_bytes


def _make_vm(repo, clock, device, mtu=185):
    vm = SyncControlViewModel(
        repo, device, device_id="dev1", now_ms=clock.now_ms, mtu_size=mtu
    )
    device.set_record_sink(vm.on_records)
    return vm


def test_start_requests_from_high_water_plus_one(repo, clock):
    repo.upsert("dev1", [make_event_bytes(seq=41), make_event_bytes(seq=42)])
    device = FakeSyncPort()
    vm = _make_vm(repo, clock, device)
    view = run(vm.start())
    assert device.requests == [(43, SYNC_REQUEST_BATCH)]  # SYNC §4.1 cursor model
    assert view.requested_from == 43
    assert view.state == sync_control.STATE_RUNNING


def test_progress_counters_ack_and_completion(repo, clock):
    device = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=2))
    vm = _make_vm(repo, clock, device)
    run(vm.start())
    device.emit_records([make_event_bytes(seq=1), make_event_bytes(seq=2)])
    view = run(vm.tick())
    assert view.received == 2
    assert view.stored == 2
    assert view.acked_seq == 2
    assert device.acks == [2]
    assert view.state == sync_control.STATE_DONE  # device head 2 <= stored 2
    assert view.status.last_synced == 2  # FakeSyncPort moved its marker exactly once


def test_duplicate_delivery_shows_up_in_counters(repo, clock):
    device = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=1))
    vm = _make_vm(repo, clock, device)
    run(vm.start())
    device.emit_records([make_event_bytes(seq=1)])
    device.emit_records([make_event_bytes(seq=1)])
    view = run(vm.tick())
    assert view.received == 2
    assert view.stored == 1  # (device_id, seq) upsert is idempotent


def test_status_flags_streaming_is_surfaced(repo, clock):
    device = FakeSyncPort(
        status=SyncStatus(linked=True, link_up=True, streaming=True, last_seq=1)
    )
    vm = _make_vm(repo, clock, device)
    run(vm.start())
    view = run(vm.tick())
    assert view.status.streaming is True  # never swallowed


def test_status_flag_mtu_too_low_is_surfaced_in_message_and_view(repo, clock):
    device = FakeSyncPort(
        status=SyncStatus(linked=True, link_up=True, last_seq=1, mtu_too_low=True)
    )
    vm = _make_vm(repo, clock, device)
    run(vm.start())
    view = run(vm.tick())
    assert view.status.mtu_too_low is True
    assert "mtu_too_low" in view.message  # mirrors SYNC §3.2 flag for the UI (§10)


def test_status_flag_transfer_aborted_is_surfaced(repo, clock):
    device = FakeSyncPort(
        status=SyncStatus(linked=True, link_up=True, last_seq=1, transfer_aborted=True)
    )
    vm = _make_vm(repo, clock, device)
    run(vm.start())
    view = run(vm.tick())
    assert view.status.transfer_aborted is True
    assert "transfer_aborted" in view.message


def test_mtu_gate_refuses_before_any_cursor_write(repo, clock):
    device = FakeSyncPort()  # linked: the gate must still refuse on MTU alone
    vm = _make_vm(repo, clock, device, mtu=23)
    view = run(vm.start())
    assert view.state == sync_control.STATE_MTU_TOO_LOW
    assert "mtu_too_low" in view.message
    assert device.requests == []  # fail closed before any CURSOR (D8, backlog 37)


def test_set_mtu_updates_the_gate(repo, clock):
    device = FakeSyncPort()
    vm = _make_vm(repo, clock, device, mtu=23)
    vm.set_mtu(55)  # exactly the floor: allowed (SYNC §2)
    view = run(vm.start())
    assert view.state == sync_control.STATE_RUNNING
    assert device.requests == [(1, SYNC_REQUEST_BATCH)]
