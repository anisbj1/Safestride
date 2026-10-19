"""§9 test 12 — the D8 fail-closed MTU gate (backlog 37): a fake ATT MTU of 23 makes
`bench sync` refuse with `mtu_too_low` text and a non-zero exit *before any CURSOR
write*; MTU >= 55 lets request_from run (from_seq = hw + 1)."""

from __future__ import annotations

from companion import bench
from companion.data.models import SyncStatus
from companion.sync_port import FakeSyncPort
from companion.transport.fake_transport import FakeTransport
from vectors import make_event_bytes


def _run_sync(tmp_path, capsys, *, mtu: int, port: FakeSyncPort):
    transport = FakeTransport(mtu=mtu)
    code = bench.main(
        ["sync", "--address", "X"],
        transport_factory=lambda: transport,
        port_factory=lambda: port,
        db_path=tmp_path / "sync.sqlite3",
        tick_delay_s=0,  # host tests never sleep (spec §9)
    )
    return code, capsys.readouterr().out


def test_low_mtu_refuses_sync_without_any_cursor_write(tmp_path, capsys):
    port = FakeSyncPort()
    code, out = _run_sync(tmp_path, capsys, mtu=23, port=port)
    assert code != 0  # non-zero: sync did not happen (D8)
    assert "mtu_too_low" in out
    assert "ATT MTU 23 < 55" in out
    # The "(no CURSOR written)" suffix exists only in bench's own pre-DB gate;
    # the view-model gate's message lacks it, so this pins the *bench* gate (audit
    # F21-AUD-03): removing bench's check still passes everything above, not this.
    assert "(no CURSOR written)" in out
    assert port.requests == []  # fail closed before any CURSOR (backlog 37 guard)


def test_mtu_at_the_floor_starts_the_transfer(tmp_path, capsys):
    port = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=0))
    code, out = _run_sync(tmp_path, capsys, mtu=55, port=port)
    assert code == bench.EXIT_OK
    assert port.requests and port.requests[0] == (1, 128)  # request_from called, hw+1
    assert "sync: done" in out


def test_mtu_above_the_floor_with_stored_events_resumes(tmp_path, capsys):
    from companion.data import db
    from companion.data.event_repository import EventRepository

    path = tmp_path / "sync.sqlite3"
    conn = db.open_db(path)
    try:
        EventRepository(conn).upsert("aabbccddeeff", [make_event_bytes(seq=5)])
    finally:
        conn.close()
    port = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, last_seq=5))
    transport = FakeTransport(mtu=185)
    code = bench.main(
        ["sync", "--address", "X"],
        transport_factory=lambda: transport,
        port_factory=lambda: port,
        db_path=path,
        tick_delay_s=0,
    )
    assert code == bench.EXIT_OK
    assert port.requests[0][0] == 6  # resume from stored high_water + 1 (SYNC §4.6)
