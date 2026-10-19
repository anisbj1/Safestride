"""anarolepsy-bench: bench CLI and M4 bench central — scan | connect | status | sync
(spec §7, D8). Every factory is injectable so the whole CLI is host-testable with
FakeTransport / FakeSyncPort and zero BLE hardware (spec §9).
"""

from __future__ import annotations

import argparse
import asyncio
from collections.abc import Awaitable, Callable, Sequence
from pathlib import Path

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.domain import wording
from companion.features import sync_control
from companion.features.sync_control import SyncControlViewModel
from companion.sync import bound_port_factory
from companion.sync_port import SyncPort
from companion.transport import ports
from companion.transport.bleak_transport import SCAN_TIMEOUT_S, BleakGattTransport, bleak_scan
from companion.transport.codec import decode_battery
from companion.transport.ports import GattTransport, ScanResult
from companion.ui import render

EXIT_OK = 0  # command completed (script-detectable convention)
EXIT_FAIL = 1  # refused or failed: MTU gate, pairing, nothing found, sync not linked

# [EXPERIMENTAL] Bench sync pump cadence (spec §8): one view-model tick per
# interval; injected as 0 in tests so host tests never sleep. Tune with bench use.
SYNC_CLI_TICK_S = 0.05
# [EXPERIMENTAL] Upper bound of CLI sync rounds so a scripted bench run always
# terminates; real long transfers will be driven by F20's client loop (D4).
SYNC_CLI_MAX_ROUNDS = 200

ScannerFactory = Callable[[float], Awaitable[list[ScanResult]]]
TransportFactory = Callable[[], GattTransport]
PortFactory = Callable[[], SyncPort]


def matches_scan(name: str, uuids: Sequence[str]) -> bool:
    """Scan filter (BLE_GATT §2): local name must match; when the backend surfaces
    advertised 128-bit UUIDs, the service UUID must be among them. No uuid info
    means the name check alone decides (platform metadata varies) `[ASSUMED]`."""
    if name != ports.DEVICE_LOCAL_NAME:
        return False
    if uuids and ports.GATT_SERVICE_UUID.lower() not in {u.lower() for u in uuids}:
        return False
    return True


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="anarolepsy-bench",
        description=(
            "Bench central for the Anarolepsy bracelet (not a medical device): "
            "scan, connect, status, sync."
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)
    scan = sub.add_parser("scan", help="discover bracelets by service UUID + local name")
    scan.add_argument("--timeout-s", type=float, default=SCAN_TIMEOUT_S)
    for name in ("connect", "status", "sync"):
        cmd = sub.add_parser(name, help=f"{name} the bracelet")
        cmd.add_argument(
            "--address", default=None, help="device address (default: first from scan)"
        )
    return parser


def main(
    argv: list[str] | None = None,
    *,
    transport_factory: TransportFactory | None = None,
    scanner_factory: ScannerFactory | None = None,
    port_factory: PortFactory | None = None,
    db_path: Path | str | None = None,
    now_ms: Callable[[], int] | None = None,
    tick_delay_s: float = SYNC_CLI_TICK_S,
) -> int:
    """Console entry point (anarolepsy-bench); injectable seams for pytest (D2/D8)."""
    args = build_parser().parse_args(argv)
    scanner_factory = scanner_factory or bleak_scan
    # W5: F20's real sync client (lib/sync) replaces the old NullSyncPort default.
    # bound_port_factory wraps the transport factory so the port factory can reach
    # the transport instance created by the command (connect-then-port ordering,
    # D12(2)). Injected port_factory/transport_factory (pytest fakes) still win.
    base_transport_factory = transport_factory or BleakGattTransport
    if port_factory is None:
        transport_factory, port_factory = bound_port_factory(base_transport_factory)
    else:
        transport_factory = base_transport_factory
    if now_ms is None:
        from companion.app import monotonic_ms

        now_ms = monotonic_ms
    return asyncio.run(
        _dispatch(
            args,
            transport_factory=transport_factory,
            scanner_factory=scanner_factory,
            port_factory=port_factory,
            db_path=db_path,
            now_ms=now_ms,
            tick_delay_s=tick_delay_s,
        )
    )


async def _dispatch(
    args: argparse.Namespace,
    *,
    transport_factory: TransportFactory,
    scanner_factory: ScannerFactory,
    port_factory: PortFactory,
    db_path: Path | str | None,
    now_ms: Callable[[], int],
    tick_delay_s: float,
) -> int:
    if args.command == "scan":
        return await _cmd_scan(args, scanner_factory=scanner_factory)
    address = await _resolve_address(args.address, scanner_factory)
    if address is None:
        print("no Anarolepsy device found — run `anarolepsy-bench scan` or pass --address")
        return EXIT_FAIL
    if args.command == "connect":
        return await _cmd_connect(address, transport_factory=transport_factory)
    if args.command == "status":
        return await _cmd_status(
            address, transport_factory=transport_factory, port_factory=port_factory
        )
    if args.command == "sync":
        return await _cmd_sync(
            address,
            transport_factory=transport_factory,
            port_factory=port_factory,
            db_path=db_path,
            now_ms=now_ms,
            tick_delay_s=tick_delay_s,
        )
    raise AssertionError(f"unhandled command {args.command!r}")


async def _resolve_address(address: str | None, scanner_factory: ScannerFactory) -> str | None:
    """Explicit --address wins; otherwise the first filtered scan hit (BLE_GATT §2)."""
    if address is not None:
        return address
    results = await scanner_factory(SCAN_TIMEOUT_S)
    for result in results:
        if matches_scan(result.name, result.uuids):
            return result.address
    return None


async def _cmd_scan(args: argparse.Namespace, *, scanner_factory: ScannerFactory) -> int:
    results = await scanner_factory(args.timeout_s)
    matching = [r for r in results if matches_scan(r.name, r.uuids)]
    print(render.render_scan(matching))
    return EXIT_OK if matching else EXIT_FAIL


async def _cmd_connect(address: str, *, transport_factory: TransportFactory) -> int:
    transport = transport_factory()
    try:
        info = await transport.connect(address)
    except PermissionError as exc:  # BondingRequiredError (D9): actionable, no retry loop
        print(f"pairing: {exc}")
        return EXIT_FAIL
    try:
        print(render.render_connect(info.device_id, transport.mtu_size))
        return EXIT_OK
    finally:
        await transport.disconnect()


async def _cmd_status(
    address: str,
    *,
    transport_factory: TransportFactory,
    port_factory: PortFactory,
) -> int:
    transport = transport_factory()
    try:
        info = await transport.connect(address)
    except PermissionError as exc:
        print(f"pairing: {exc}")
        return EXIT_FAIL
    try:
        battery = decode_battery(await transport.read(ports.BATTERY_LEVEL_UUID))
        print(
            render.render_status(
                device_id=info.device_id,
                firmware=info.fw,
                battery_text=wording.battery_text(battery.percent),
                mtu=transport.mtu_size,
                sync_status=port_factory().status(),
            )
        )
        return EXIT_OK
    finally:
        await transport.disconnect()


async def _cmd_sync(
    address: str,
    *,
    transport_factory: TransportFactory,
    port_factory: PortFactory,
    db_path: Path | str | None,
    now_ms: Callable[[], int],
    tick_delay_s: float,
) -> int:
    transport = transport_factory()
    try:
        info = await transport.connect(address)
    except PermissionError as exc:
        print(f"pairing: {exc}")
        return EXIT_FAIL
    try:
        mtu = transport.mtu_size
        if mtu < ports.MIN_SYNC_MTU_BYTES:
            # Fail closed before any CURSOR write (D8, backlog 37, spec §10).
            print(
                f"mtu_too_low: ATT MTU {mtu} < {ports.MIN_SYNC_MTU_BYTES} — sync refused "
                "(no CURSOR written)"
            )
            return EXIT_FAIL
        path = db_path if db_path is not None else db.default_db_path()
        conn = db.open_db(path)
        try:
            repository = EventRepository(conn)
            port = port_factory()
            view_model = SyncControlViewModel(
                repository, port, device_id=info.device_id, now_ms=now_ms, mtu_size=mtu
            )
            port.set_record_sink(view_model.on_records)
            view = await view_model.start()
            rounds = 0
            while (
                view.state in (sync_control.STATE_RUNNING, sync_control.STATE_STALLED)
                and rounds < SYNC_CLI_MAX_ROUNDS
            ):
                if tick_delay_s > 0:
                    await asyncio.sleep(tick_delay_s)
                view = await view_model.tick()
                rounds += 1
            print(render.render_sync(view))
            if view.state in (sync_control.STATE_NOT_LINKED, sync_control.STATE_MTU_TOO_LOW):
                return EXIT_FAIL
            return EXIT_OK
        finally:
            conn.close()
    finally:
        await transport.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())
