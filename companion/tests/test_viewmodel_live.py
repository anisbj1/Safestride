"""§9 test 8 — live view-model over scripted bytes: battery 87, state Drowsy,
HR 72, link/sync status; disconnect retains values **with age** + not-live banner
(spec §10); battery 0xFF renders "unknown", never a percentage."""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.features.live import LiveViewModel
from companion.sync_port import FakeSyncPort
from companion.transport.fake_transport import FakeTransport
from companion.transport.ports import (
    BATTERY_LEVEL_UUID,
    HEART_RATE_UUID,
    LIVE_STATUS_UUID,
)
from conftest import run
from vectors import V1_LIVE_STATUS, V2_HR_UINT8_72


def _make_vm(clock, *, mtu: int = 185):
    transport = FakeTransport(
        mtu=mtu,
        reads={
            LIVE_STATUS_UUID: V1_LIVE_STATUS,
            BATTERY_LEVEL_UUID: bytes([87]),
            HEART_RATE_UUID: V2_HR_UINT8_72,
        },
    )
    port = FakeSyncPort(
        status=SyncStatus(
            linked=True, link_up=True, streaming=True, last_seq=9, last_synced=7
        )
    )
    vm = LiveViewModel(now_ms=clock.now_ms, sync_port=port)

    async def drive():
        await transport.connect(None)
        vm.on_live_status(await transport.read(LIVE_STATUS_UUID))
        vm.on_battery(await transport.read(BATTERY_LEVEL_UUID))
        vm.on_hr(await transport.read(HEART_RATE_UUID))

    run(drive())
    vm.set_connected(True)
    return vm, transport


def test_scripted_bytes_produce_the_live_view(clock):
    vm, _ = _make_vm(clock)
    state = vm.snapshot()
    assert state.connected
    assert state.state_text == "Drowsy"
    assert state.wear_text == "Worn"
    assert state.activity_text == "Sitting"
    assert "87%" in state.battery_text
    assert "72 bpm" in state.hr_text
    assert "streaming" in state.sync_text and "last_seq=9" in state.sync_text
    assert state.banner == ""
    assert not state.stale


def test_disconnect_retains_values_with_age_and_not_live_banner(clock):
    vm, _ = _make_vm(clock)
    vm.set_connected(False)
    clock.advance(2000)
    state = vm.snapshot()
    assert state.state_text == "Drowsy"  # retained, never cleared
    assert "87%" in state.battery_text
    assert "2s ago" in state.battery_text  # every cached value carries its age (§10)
    assert "not live" in state.banner
    assert "2s ago" in state.banner


def test_stale_label_beyond_the_threshold(clock):
    vm, _ = _make_vm(clock)
    vm.set_connected(False)
    clock.advance(6000)  # > kStaleLabelMs = 5000 (spec §8)
    state = vm.snapshot()
    assert "(stale)" in state.battery_text
    assert state.stale


def test_battery_0xff_is_unknown_never_a_percentage(clock):
    transport = FakeTransport(reads={BATTERY_LEVEL_UUID: b"\xff"})
    vm = LiveViewModel(now_ms=clock.now_ms)

    async def drive():
        await transport.connect(None)
        vm.on_battery(await transport.read(BATTERY_LEVEL_UUID))

    run(drive())
    assert "unknown" in vm.snapshot().battery_text
    assert "%" not in vm.snapshot().battery_text.split("(")[0]


def test_empty_live_status_keeps_prior_state_and_reports_no_snapshot(clock):
    transport = FakeTransport(reads={LIVE_STATUS_UUID: b""})  # BLE_GATT §5 empty value
    vm = LiveViewModel(now_ms=clock.now_ms)

    async def drive():
        await transport.connect(None)
        vm.on_live_status(await transport.read(LIVE_STATUS_UUID))

    run(drive())
    state = vm.snapshot()
    assert state.state_text == "no snapshot yet"
    assert state.sync_text == "no sync client"  # port optional (D2)


def test_unlinked_sync_port_reports_not_linked(clock):
    from companion.sync_port import NullSyncPort

    vm = LiveViewModel(now_ms=clock.now_ms, sync_port=NullSyncPort())
    assert vm.snapshot().sync_text == "sync client not linked"
