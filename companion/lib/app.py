"""anarolepsy-app: composition root + user console commands (spec §7, D3 CLI-first).

Wires transport + repository + view-models + ui — no logic of its own. Subcommands:
  watch     live status refresh loop (1 Hz), time sync on connect
  timeline  read-only synced timeline over the local DB
  sync      sync the device over BLE into the local DB (F20 real SyncPort)
  history   read-only sleep/nap history from the local DB (F16)
  explain   explain one synced event, reasons + confidence (F22)
  settime   explicit SetTime write (time sync, external.md §6.1)
The bench CLI (scan | connect | status | sync) lives in companion.bench.
"""

from __future__ import annotations

import argparse
import asyncio
import sqlite3
import time
from collections.abc import Callable
from pathlib import Path

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.features import explain as explain_mod
from companion.features import history as history_mod
from companion.features import live as live_mod
from companion.features import timeline as timeline_mod
from companion.features.timeline import TimelineViewModel
from companion.sync import runner as sync_runner
from companion.transport import commands, ports
from companion.transport.bleak_transport import BleakGattTransport
from companion.transport.codec import CodecError
from companion.transport.ports import GattTransport
from companion.ui import render

# [DERIVED] Set the device clock on every connect (spec §8 kSetTimeOnConnect): the
# device clock is unknown after boot (DEC-009) and F17's resync tolerance makes
# repeats harmless — the timeline's wall-time reconstruction depends on it
# (external.md §6.1). Opt out with --no-set-time.
SET_TIME_ON_CONNECT = True


def monotonic_ms() -> int:
    """Default clock: monotonic milliseconds (core.md §1 TimeMs semantics)."""
    return int(time.monotonic() * 1000)


def unix_seconds_now() -> int:
    """Default wall clock for SetTime (UnixSeconds, core.md §1; 0 = unknown)."""
    return int(time.time())


def main(
    argv: list[str] | None = None,
    *,
    transport_factory: Callable[[], GattTransport] | None = None,
    db_path: Path | str | None = None,
    now_ms: Callable[[], int] | None = None,
    now_unix: Callable[[], int] | None = None,
) -> int:
    """Console entry point (anarolepsy-app). Factories are injectable for tests (D2)."""
    transport_factory = transport_factory or BleakGattTransport
    now_ms = now_ms or monotonic_ms
    now_unix = now_unix or unix_seconds_now
    parser = _build_parser()
    # Delegating subcommands (sync/history/explain) parse their own flags inside
    # each half, so we use parse_known_args and forward the leftovers. Fixed-arg
    # subcommands (watch/timeline/settime) get an explicit error if they stray.
    args, rest = parser.parse_known_args(argv)
    if args.command == "watch":
        return asyncio.run(
            _cmd_watch(args, transport_factory=transport_factory, now_ms=now_ms, now_unix=now_unix)
        )
    if args.command == "timeline":
        return _cmd_timeline(args, db_path=db_path)
    if args.command == "sync":
        # F20 real SyncPort over BLE (D12(3)); injectable seams for host tests.
        return sync_runner.main(
            rest,
            transport_factory=transport_factory,
            db_path=db_path,
            now_ms=now_ms,
        )
    if args.command == "history":
        # F16 read-only history view over the local DB (D15 builder + main argv).
        return history_mod.main(rest)
    if args.command == "explain":
        # F22 explain view over the local DB (D7 builder + main argv).
        return explain_mod.main(rest, db_path=db_path)
    if args.command == "settime":
        return asyncio.run(
            _cmd_settime(args, transport_factory=transport_factory, now_unix=now_unix)
        )
    if rest:
        parser.error(f"unrecognized arguments: {' '.join(rest)}")
    parser.error(f"unknown command {args.command!r}")
    return 2


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="anarolepsy-app",
        description=(
            "Local-first companion console for the Anarolepsy bracelet "
            "(not a medical device; observes only)."
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)
    watch = sub.add_parser("watch", help="live status refresh loop")
    watch.add_argument("--address", default=None, help="device address (see bench scan)")
    watch.add_argument("--once", action="store_true", help="print a single frame and exit")
    watch.add_argument(
        "--interval-ms",
        type=int,
        default=live_mod.LIVE_STATUS_INTERVAL_MS,
        help="refresh cadence (device emits <= 1 Hz, BLE_GATT §4.1)",
    )
    watch.add_argument("--no-set-time", action="store_true", help="skip SetTime on connect")
    timeline = sub.add_parser("timeline", help="read-only synced timeline (local DB)")
    timeline.add_argument("--limit", type=int, default=timeline_mod.DEFAULT_TIMELINE_LIMIT)
    timeline.add_argument("--device", default=None, help="filter by device_id")
    # W5 halves delegate to their own subparser: each feature owns its flags.
    # We capture argv after the subcommand token and hand it to the half's own
    # parser (sync/history/explain each call parse_args on their own argv).
    sub.add_parser("sync", help="sync device over BLE into the local DB (F20)")
    sub.add_parser("history", help="sleep/nap history from local DB (F16)")
    sub.add_parser("explain", help="explain one synced event (F22)")
    settime = sub.add_parser("settime", help="send SetTime(unix_seconds) to the device")
    settime.add_argument("--address", default=None)
    settime.add_argument(
        "--unix", type=int, default=None, help="unix seconds (default: this computer's clock)"
    )
    return parser


async def _cmd_watch(
    args: argparse.Namespace,
    *,
    transport_factory: Callable[[], GattTransport],
    now_ms: Callable[[], int],
    now_unix: Callable[[], int],
) -> int:
    transport = transport_factory()
    view_model = live_mod.LiveViewModel(now_ms=now_ms)
    try:
        info = await transport.connect(args.address)
        if SET_TIME_ON_CONNECT and not args.no_set_time:
            await transport.write(ports.COMMAND_UUID, commands.build_set_time(now_unix()))
        await transport.start_notify(ports.HEART_RATE_UUID, view_model.on_hr)
        view_model.set_connected(True)
        while True:
            view_model.on_live_status(await transport.read(ports.LIVE_STATUS_UUID))
            view_model.on_battery(await transport.read(ports.BATTERY_LEVEL_UUID))
            print(f"[{info.device_id}]")
            print(render.render_live(view_model.snapshot()))
            if args.once:
                break
            await asyncio.sleep(max(1, args.interval_ms) / 1000)
        return 0
    except CodecError as exc:
        # Unknown version byte / malformed width: rejected, never coerced (spec §5).
        print(f"payload rejected: {exc}")
        return 1
    except PermissionError as exc:  # BondingRequiredError (D9): actionable, no retry loop
        print(f"pairing: {exc}")
        return 1
    finally:
        view_model.set_connected(False)
        await transport.disconnect()


def _cmd_timeline(args: argparse.Namespace, *, db_path: Path | str | None) -> int:
    path = db_path if db_path is not None else db.default_db_path()
    conn = db.open_db(path)
    try:
        repository = EventRepository(conn)
        view_model = TimelineViewModel(
            repository, device_id=args.device, limit=args.limit
        )
        print(render.render_timeline(view_model.rows()))
    except sqlite3.Error as exc:  # locked/corrupt: report, stay responsive (§10)
        print(f"database error: {exc}")
        return 1
    finally:
        conn.close()
    return 0


async def _cmd_settime(
    args: argparse.Namespace,
    *,
    transport_factory: Callable[[], GattTransport],
    now_unix: Callable[[], int],
) -> int:
    transport = transport_factory()
    try:
        await transport.connect(args.address)
        value = args.unix if args.unix is not None else now_unix()
        await transport.write(ports.COMMAND_UUID, commands.build_set_time(value))
        print(f"SetTime sent: {value} unix seconds")
        return 0
    except ValueError as exc:  # out-of-u32 range: refused before any write
        print(f"settime refused: {exc}")
        return 1
    except PermissionError as exc:
        print(f"pairing: {exc}")
        return 1
    finally:
        await transport.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())
