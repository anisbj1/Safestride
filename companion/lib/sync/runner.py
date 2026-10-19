"""F20 runner — the real long-transfer driver (D12(3)).

`bench.py` line 31 defers "real long transfers" to this module: a thin
composition fragment (connect → MTU gate → DB → view-model loop) that reuses
F21's `SyncControlViewModel` as the **single** orchestration logic (D11: no
second recovery loop anywhere). P4' may instead call `run_sync` from `app.py`;
`app.py`/`README.md`/`pyproject.toml` are never edited by this feature.
"""

from __future__ import annotations

import argparse
import asyncio
from collections.abc import Awaitable, Callable, Sequence
from pathlib import Path

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.features import sync_control
from companion.features.sync_control import SyncControlViewModel, SyncViewState
from companion.sync.client import SyncClient
from companion.sync_port import SyncPort
from companion.transport import ports
from companion.transport.bleak_transport import SCAN_TIMEOUT_S, BleakGattTransport, bleak_scan
from companion.transport.ports import GattTransport, ScanResult
from companion.ui import render

EXIT_OK = 0  # command completed (script-detectable convention, mirrors bench)
EXIT_FAIL = 1  # refused or failed: MTU gate, pairing, sync not linked

# [EXPERIMENTAL] Runner sync pump cadence (spec §8): one view-model tick per
# interval; injected as 0 in tests so host tests never sleep. Tune with bench use.
SYNC_RUN_TICK_S = 0.05
# [EXPERIMENTAL] Upper bound of sync rounds so a scripted run always terminates;
# 2000 >> bench's 200-round *scripted* bound — covers a real transfer (spec §8).
SYNC_RUN_MAX_ROUNDS = 2000

ScannerFactory = Callable[[float], Awaitable[list[ScanResult]]]
TransportFactory = Callable[[], GattTransport]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="anarolepsy-sync",
        description=(
            "Sync driver for the Anarolepsy bracelet (not a medical device): "
            "transfer every unsynced event into the local companion database."
        ),
    )
    parser.add_argument(
        "--address", default=None, help="device address (default: first from scan)"
    )
    return parser


def matches_scan(name: str, uuids: Sequence[str]) -> bool:
    """Scan filter (BLE_GATT §2), same rule as bench: local name + advertised
    service UUID when the backend surfaces it (no uuid info = name decides)."""
    if name != ports.DEVICE_LOCAL_NAME:
        return False
    if uuids and ports.GATT_SERVICE_UUID.lower() not in {u.lower() for u in uuids}:
        return False
    return True


async def run_sync(
    port: SyncPort,
    repository: EventRepository,
    *,
    device_id: str,
    now_ms: Callable[[], int],
    mtu_size: int,
    tick_delay_s: float = SYNC_RUN_TICK_S,
    max_rounds: int = SYNC_RUN_MAX_ROUNDS,
) -> SyncViewState:
    """Drive `SyncControlViewModel.start()` + bounded `tick()` loop (single
    orchestration source, D11/D12); returns the final view state."""
    view_model = SyncControlViewModel(
        repository, port, device_id=device_id, now_ms=now_ms, mtu_size=mtu_size
    )
    port.set_record_sink(view_model.on_records)
    view = await view_model.start()
    rounds = 0
    while (
        view.state in (sync_control.STATE_RUNNING, sync_control.STATE_STALLED)
        and rounds < max_rounds
    ):
        if tick_delay_s > 0:
            await asyncio.sleep(tick_delay_s)
        view = await view_model.tick()
        rounds += 1
    return view


async def _resolve_address(
    address: str | None, scanner_factory: ScannerFactory
) -> str | None:
    """Explicit --address wins; otherwise the first filtered scan hit."""
    if address is not None:
        return address
    for result in await scanner_factory(SCAN_TIMEOUT_S):
        if matches_scan(result.name, result.uuids):
            return result.address
    return None


async def _run(
    args: argparse.Namespace,
    *,
    transport_factory: TransportFactory,
    scanner_factory: ScannerFactory,
    db_path: Path | str | None,
    now_ms: Callable[[], int],
    tick_delay_s: float,
    max_rounds: int,
) -> int:
    address = await _resolve_address(args.address, scanner_factory)
    if address is None:
        print("no Anarolepsy device found — pass --address or run `anarolepsy-bench scan`")
        return EXIT_FAIL
    transport = transport_factory()
    try:
        info = await transport.connect(address)
    except PermissionError as exc:  # bonding (D9 of F21): actionable, no retry loop
        print(f"pairing: {exc}")
        return EXIT_FAIL
    try:
        mtu = transport.mtu_size
        if mtu < ports.MIN_SYNC_MTU_BYTES:
            # Fail closed before any CURSOR write (D8 gate, spec §10): the port
            # is never even constructed, so nothing can be written.
            print(
                f"mtu_too_low: ATT MTU {mtu} < {ports.MIN_SYNC_MTU_BYTES} — sync refused "
                "(no CURSOR written)"
            )
            return EXIT_FAIL
        path = db_path if db_path is not None else db.default_db_path()
        conn = db.open_db(path)
        try:
            repository = EventRepository(conn)
            port = SyncClient(transport)  # D12(1) semantics: bound to this transport
            view = await run_sync(
                port,
                repository,
                device_id=info.device_id,
                now_ms=now_ms,
                mtu_size=mtu,
                tick_delay_s=tick_delay_s,
                max_rounds=max_rounds,
            )
            print(render.render_sync(view))
            if view.state in (
                sync_control.STATE_NOT_LINKED,
                sync_control.STATE_MTU_TOO_LOW,
            ):
                return EXIT_FAIL
            return EXIT_OK
        finally:
            conn.close()
    finally:
        await transport.disconnect()


def main(
    argv: list[str] | None = None,
    *,
    transport_factory: TransportFactory | None = None,
    scanner_factory: ScannerFactory | None = None,
    db_path: Path | str | None = None,
    now_ms: Callable[[], int] | None = None,
    tick_delay_s: float = SYNC_RUN_TICK_S,
    max_rounds: int = SYNC_RUN_MAX_ROUNDS,
) -> int:
    """Console entry point; injectable seams for pytest (D12(3), no BLE needed)."""
    args = build_parser().parse_args(argv)
    transport_factory = transport_factory or BleakGattTransport
    scanner_factory = scanner_factory or bleak_scan
    if now_ms is None:
        from companion.app import monotonic_ms

        now_ms = monotonic_ms
    return asyncio.run(
        _run(
            args,
            transport_factory=transport_factory,
            scanner_factory=scanner_factory,
            db_path=db_path,
            now_ms=now_ms,
            tick_delay_s=tick_delay_s,
            max_rounds=max_rounds,
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
