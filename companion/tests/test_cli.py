"""§9 test 11 — bench CLI: scan filters by service UUID + local name; status prints
device_id/battery/MTU/SyncStatus; connect prints the negotiated MTU. Plus the D9
bonding-error text and thin app smoke checks (composition root, spec §7)."""

from __future__ import annotations

import pytest
from companion import app, bench
from companion.data import db
from companion.data.event_repository import EventRepository
from companion.data.models import DeviceInfo, SyncStatus
from companion.sync_port import FakeSyncPort
from companion.transport import ports
from companion.transport.bleak_transport import (
    BondingRequiredError,
    _translate,
    is_bonding_required,
)
from companion.transport.commands import build_set_time
from companion.transport.fake_transport import FakeTransport
from companion.transport.ports import ScanResult
from vectors import V1_LIVE_STATUS, make_event_bytes

MATCHING_UUID = ports.GATT_SERVICE_UUID


async def _fake_scan(timeout_s: float) -> list[ScanResult]:
    return [
        ScanResult(ports.DEVICE_LOCAL_NAME, "AA:00", (MATCHING_UUID,)),  # match
        ScanResult(ports.DEVICE_LOCAL_NAME, "AA:01", ("00001234-0000-1000-8000-00805f9b34fb",)),
        ScanResult("OtherWearable", "AA:02", (MATCHING_UUID,)),
        ScanResult(ports.DEVICE_LOCAL_NAME, "AA:03"),  # backend surfaced no uuid info
    ]


def test_scan_filters_by_service_uuid_and_local_name(capsys):
    code = bench.main(["scan"], scanner_factory=_fake_scan)
    out = capsys.readouterr().out
    assert code == bench.EXIT_OK
    assert "AA:00" in out and "AA:03" in out  # right name (+ uuid when known)
    assert "AA:01" not in out  # wrong service UUID
    assert "AA:02" not in out  # wrong local name


def test_scan_with_no_matches_is_nonzero(capsys):
    async def empty_scan(timeout_s: float) -> list[ScanResult]:
        return [ScanResult("OtherWearable", "BB:00", ())]

    assert bench.main(["scan"], scanner_factory=empty_scan) == bench.EXIT_FAIL
    assert "no devices found" in capsys.readouterr().out


def test_connect_prints_negotiated_mtu(capsys):
    transport = FakeTransport(mtu=185, device_info=DeviceInfo(device_id="aabbccddeeff", fw="fw1"))
    code = bench.main(["connect", "--address", "X"], transport_factory=lambda: transport)
    out = capsys.readouterr().out
    assert code == bench.EXIT_OK
    assert "aabbccddeeff" in out
    assert "ATT MTU : 185" in out
    assert transport.connect_calls == ["X"]


def test_status_prints_identity_battery_mtu_and_sync_status(capsys):
    transport = FakeTransport(
        mtu=185,
        device_info=DeviceInfo(device_id="aabbccddeeff", fw="fw1"),
        reads={ports.BATTERY_LEVEL_UUID: bytes([55])},
    )
    device = FakeSyncPort(
        status=SyncStatus(linked=True, link_up=True, last_seq=100, last_synced=32)
    )
    code = bench.main(
        ["status", "--address", "X"],
        transport_factory=lambda: transport,
        port_factory=lambda: device,
    )
    out = capsys.readouterr().out
    assert code == bench.EXIT_OK
    assert "device  : aabbccddeeff" in out
    assert "firmware: fw1" in out
    assert "battery : 55%" in out
    assert "ATT MTU : 185" in out
    assert "last_seq=100" in out and "last_synced=32" in out


def test_status_battery_unknown_is_reported_as_unknown(capsys):
    transport = FakeTransport(reads={ports.BATTERY_LEVEL_UUID: b"\xff"})
    code = bench.main(["status", "--address", "X"], transport_factory=lambda: transport)
    assert code == bench.EXIT_OK
    assert "battery : unknown" in capsys.readouterr().out


def test_connect_without_device_reports_actionably(capsys):
    async def empty_scan(timeout_s: float) -> list[ScanResult]:
        return []

    code = bench.main(["connect"], scanner_factory=empty_scan)
    assert code == bench.EXIT_FAIL
    assert "scan" in capsys.readouterr().out  # points at the fix, no crash


def test_bonding_error_text_is_actionable():
    assert is_bonding_required(
        "Operation failed with ATT error: Insufficient Authentication (0x0F)"
    )
    assert is_bonding_required("att err 0x05: insufficient encryption")
    assert not is_bonding_required("device disconnected")
    translated = _translate(Exception("Insufficient Encryption (0x05)"))
    assert isinstance(translated, BondingRequiredError)
    assert "pair in the OS Bluetooth settings" in str(translated)


def test_bench_connect_surfaces_bonding_required(capsys):
    transport = FakeTransport(
        connect_error=BondingRequiredError("bonding required — pair in the OS settings, then retry")
    )
    code = bench.main(["connect", "--address", "X"], transport_factory=lambda: transport)
    assert code == bench.EXIT_FAIL
    assert "pairing:" in capsys.readouterr().out


# --- thin composition-root smoke checks (app.py) ------------------------------


def test_app_timeline_renders_synced_events(tmp_path, capsys):
    path = tmp_path / "app.sqlite3"
    conn = db.open_db(path)
    try:
        EventRepository(conn).upsert(
            "dev1", [make_event_bytes(seq=1, type=1, confidence=0.7, t_start_unix=1_760_000_000)]
        )
    finally:
        conn.close()
    code = app.main(["timeline"], db_path=path)
    out = capsys.readouterr().out
    assert code == 0
    assert "possible drowsiness (confidence 0.70)" in out
    assert "#1" in out


def test_app_watch_once_renders_live_and_sets_time_on_connect(capsys):
    transport = FakeTransport(
        reads={ports.LIVE_STATUS_UUID: V1_LIVE_STATUS, ports.BATTERY_LEVEL_UUID: bytes([87])}
    )
    code = app.main(
        ["watch", "--once"],
        transport_factory=lambda: transport,
        now_unix=lambda: 1_760_000_000,
    )
    out = capsys.readouterr().out
    assert code == 0
    assert "Drowsy" in out and "87%" in out
    # kSetTimeOnConnect (spec §8): the clock is set on connect, CR-021 layout.
    assert (ports.COMMAND_UUID, build_set_time(1_760_000_000)) in transport.writes


def test_app_settime_writes_the_golden_frame(capsys):
    transport = FakeTransport()
    code = app.main(["settime", "--unix", "1760000000"], transport_factory=lambda: transport)
    assert code == 0
    assert transport.writes == [(ports.COMMAND_UUID, build_set_time(1760000000))]
    assert "SetTime sent: 1760000000" in capsys.readouterr().out


@pytest.mark.parametrize("bad_unix", [-1, 2**32])
def test_app_settime_rejects_out_of_range(bad_unix):
    transport = FakeTransport()
    assert app.main(["settime", "--unix", str(bad_unix)], transport_factory=lambda: transport) == 1
    assert transport.writes == []  # refused before any write


# --- W5 H8: cross-feature dataflow dispatch (integrator, integration/wave-5) ---
# app.py is the composition root; each delegating subcommand must route to its
# real feature main() (F16 history / F22 explain / F20 sync) over a seeded local
# DB. These are the phone-side dataflow-with-real-neighbors checks (docs/05 H8).


def test_app_history_dispatches_to_f16_view(tmp_path, capsys, monkeypatch):
    path = tmp_path / "app.sqlite3"
    conn = db.open_db(path)
    try:
        # A closed MainSleep episode (F08 payload: flags=0, class=3) is what F16
        # history aggregates - a bare drowsiness event forms no episode.
        EventRepository(conn).upsert(
            "dev1",
            [
                make_event_bytes(
                    seq=1,
                    t_start_unix=1_760_000_000,
                    type=0,  # EVENT_TYPE_SLEEP_EPISODE
                    detector_id=8,  # SLEEP_DETECTOR_ID_MIRROR
                    payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]),
                    confidence=0.7,
                )
            ],
        )
    finally:
        conn.close()
    monkeypatch.setenv(db.ENV_DB_PATH, str(path))  # F16 history resolves via env
    # --days large enough that the 2025 test event is inside the window.
    code = app.main(["history", "--days", "100000"])
    out = capsys.readouterr().out
    assert code == 0
    assert "MainSleep" in out or "main sleep" in out.lower()  # F16 rendered the episode


def test_app_history_empty_db_is_honest_not_fabricated(tmp_path, capsys, monkeypatch):
    path = tmp_path / "empty.sqlite3"
    db.open_db(path).close()
    monkeypatch.setenv(db.ENV_DB_PATH, str(path))
    code = app.main(["history"])
    out = capsys.readouterr().out
    assert code == 0
    assert "no history yet" in out  # unknown stays a valid output (00), never fabricated


def test_app_explain_dispatches_to_f22_view(tmp_path, capsys):
    path = tmp_path / "app.sqlite3"
    conn = db.open_db(path)
    try:
        EventRepository(conn).upsert(
            "dev1", [make_event_bytes(seq=1, type=1, confidence=0.7, t_start_unix=1_760_000_000)]
        )
    finally:
        conn.close()
    code = app.main(["explain", "--seq", "1", "--device", "dev1"], db_path=path)
    out = capsys.readouterr().out
    assert code == 0
    assert "possible drowsiness" in out and "confidence" in out  # F22: possible + confidence


def test_app_sync_dispatches_to_f20_runner(tmp_path, capsys):
    # The W5 sync seam: app sync -> F20 runner.main over a device-wired transport.
    # MTU 255 > floor 55, so the runner reaches `done` and stores every event.
    from companion.sync.tests.fake_device import make_device_link
    from companion.sync.tests.sync_test_support import FakeClock, make_event48

    clock = FakeClock()
    events = {
        seq: make_event48(seq, confidence=0.8, t_start_unix=1_760_000_000) for seq in (1, 2, 3)
    }
    holder: dict[str, object] = {}

    def factory():
        device, transport = make_device_link(events, now_ms=clock.now_ms, mtu=255)
        transport.auto_pump = True
        holder["transport"] = transport
        return transport

    path = tmp_path / "app.sqlite3"
    code = app.main(
        ["sync", "--address", "AA:BB"],
        transport_factory=factory,
        db_path=path,
        now_ms=clock.now_ms,
    )
    out = capsys.readouterr().out
    assert code == 0
    assert "sync: done" in out  # routed to F20's runner, not a stub
    conn = db.open_db(path)
    try:
        repo = EventRepository(conn)
        device_id = holder["transport"].device_info.device_id
        assert repo.event_count(device_id) == 3  # the sync really reached the DB
    finally:
        conn.close()
