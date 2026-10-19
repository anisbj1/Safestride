"""tools/capture CLI — record a research capture into a session directory.

Usage (developer action A3, spec §15):

    python tools/capture/cli.py --port COM5 --out D:\\records
    python tools/capture/cli.py --stdin capture.log --out /tmp/records

Stop with Ctrl-C: the session is finalized with `end_reason: user` and the
summary prints the directory plus the drop counters from the last #HDR —
`raw_drops: 0` is the sanity check of §15 A3. Diagnostics ([PIPE]/[BTN]/...)
are diverted to diagnostics.log and never enter the dataset (F28 D6).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

if __package__ in (None, ""):  # allow `python tools/capture/cli.py ...` from anywhere
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from capture import lines as linemod  # noqa: E402 - after the path shim above
from capture.session import SessionManager  # noqa: E402
from capture.transport import DEFAULT_BAUD, FileTransport, SerialTransport  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Record a bracelet research capture.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    source.add_argument("--stdin", metavar="FILE", help="read a saved capture ('-' = stdin)")
    parser.add_argument("--out", required=True, help="directory that receives research_<id>/")
    parser.add_argument("--duration", type=float, default=None, help="stop after N seconds")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="serial baud rate")
    parser.add_argument("--marker", default=None, help="host-side marker text for this session")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    manager = SessionManager(args.out)
    if args.marker:
        manager.add_host_marker(args.marker)

    transport: FileTransport | SerialTransport
    if args.port:
        try:
            transport = SerialTransport(args.port, baudrate=args.baud)
        except Exception as exc:  # serial raises several exception types
            print(f"cannot open {args.port}: {exc}", file=sys.stderr)
            return 2
    else:
        transport = FileTransport(args.stdin)

    end_reason = "eof"
    started = time.monotonic()
    try:
        for line in transport.lines():
            if line is None:  # serial timeout tick
                if args.duration and time.monotonic() - started >= args.duration:
                    end_reason = "user"
                    break
                continue
            try:
                manager.feed_line(line)
            except linemod.SchemaMismatch as exc:
                print(f"stopping: {exc}", file=sys.stderr)
                end_reason = "schema_change"
                break
            if args.duration and time.monotonic() - started >= args.duration:
                end_reason = "user"
                break
    except KeyboardInterrupt:
        end_reason = "user"
    finally:
        session = manager.session
        directory = manager.finalize(end_reason)

    print(f"session: {directory}")
    if session is not None:
        print(
            f"rows: {session.row_counts} | malformed: {session.malformed_rows} | "
            f"raw_drops: {session.raw_drops} "
            f"(imu={session.imu_drops} ppg={session.ppg_drops} "
            f"line={session.line_drops} feat_missed={session.feature_missed}) | "
            f"diagnostics: {session.diagnostics_lines}"
        )
        if session.imu_period_ms is not None and session.imu_missing_samples is not None:
            print(
                f"imu gap cross-check: period {session.imu_period_ms} ms, "
                f"{session.imu_missing_samples} missing samples (from t_ms gaps, "
                f"independent of device counters)"
            )
        if session.raw_drops:
            print(
                "WARNING: raw_drops > 0 - replay comparison is invalid "
                "(tools/replay/compare.py exits 2); samples were lost on device "
                "before the UART (see F28 status log)",
                file=sys.stderr,
            )
        if session.malformed_rows:
            print(
                f"WARNING: {session.malformed_rows} research rows were torn/malformed "
                "(shared-UART interleaving with bench prints); fragments are in "
                "diagnostics.log - keep the bench loop quiet during captures",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
