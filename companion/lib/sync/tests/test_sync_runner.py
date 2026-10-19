"""CT24–CT25: runner entrypoints (D12(3)) — happy path exits 0 with `done`;
ATT MTU below the floor exits 1 with **zero** CURSOR writes (fail closed,
spec §9/§10, mirrors bench's D8 gate)."""

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.sync import runner
from companion.transport.ports import SYNC_CONTROL_UUID

from .fake_device import make_device_link
from .sync_test_support import FakeClock, make_event48


def _wired_factory(events: dict[int, bytes], clock: FakeClock, *, mtu: int = 255):
    """A bench-like `transport_factory` that returns a device-wired transport.

    `auto_pump=True` models a device that reacts right after it processes a
    request (the runner has no harness round of its own)."""

    def factory():
        device, transport = make_device_link(events, now_ms=clock.now_ms, mtu=mtu)
        transport.auto_pump = True
        return transport

    return factory


def test_CT24_runner_main_happy_path_exits_zero_done(tmp_path, capsys) -> None:
    clock = FakeClock()
    events = {seq: make_event48(seq) for seq in range(1, 25)}
    holder: dict[str, object] = {}

    def factory():
        transport = _wired_factory(events, clock)()
        holder["transport"] = transport
        return transport

    code = runner.main(
        ["--address", "AA:BB"],
        transport_factory=factory,
        db_path=tmp_path / "sync.sqlite3",
        now_ms=clock.now_ms,
        tick_delay_s=0,
    )
    assert code == 0
    assert "sync: done" in capsys.readouterr().out
    transport = holder["transport"]
    assert transport.connected is False  # the runner disconnects (finally)
    conn = db.open_db(tmp_path / "sync.sqlite3")
    try:
        repo = EventRepository(conn)
        device_id = transport.device_info.device_id
        assert repo.event_count(device_id) == 24
        assert repo.high_water(device_id) == 24
    finally:
        conn.close()


def test_CT25_runner_mtu_gate_fails_closed_no_cursor(tmp_path, capsys) -> None:
    clock = FakeClock()
    events = {seq: make_event48(seq) for seq in range(1, 25)}
    holder: dict[str, object] = {}

    def factory():
        transport = _wired_factory(events, clock, mtu=23)()
        holder["transport"] = transport
        return transport

    code = runner.main(
        ["--address", "AA:BB"],
        transport_factory=factory,
        db_path=tmp_path / "sync.sqlite3",
        now_ms=clock.now_ms,
        tick_delay_s=0,
    )
    assert code == 1
    assert "mtu_too_low" in capsys.readouterr().out
    transport = holder["transport"]
    assert transport.device.cursors == []  # the device never saw a CURSOR
    assert all(char != SYNC_CONTROL_UUID for char, _ in transport.writes)
    conn = db.open_db(tmp_path / "sync.sqlite3")
    try:
        repo = EventRepository(conn)
        assert repo.event_count(transport.device_info.device_id) == 0  # nothing stored
    finally:
        conn.close()


def test_CT25_runner_bound_port_factory_orders_transport_first() -> None:
    """D12(2) adapter: honest failure if the port is built before the transport."""
    from companion.sync import bound_port_factory

    clock = FakeClock()
    events = {seq: make_event48(seq) for seq in range(1, 25)}
    make_transport, make_port = bound_port_factory(
        lambda: _wired_factory(events, clock)()
    )
    try:
        make_port()
        raise AssertionError("expected RuntimeError before the transport exists")
    except RuntimeError as exc:
        assert "transport" in str(exc)
    transport = make_transport()
    port = make_port()
    assert port._transport is transport  # binding the live transport is the point
