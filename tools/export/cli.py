"""tools/export CLI — export recorded session dirs into schema-annotated CSVs.

Usage (F28 spec §16):

    python tools/export.cli C:\\captures\\research_20261011T101500Z --out D:\\datasets
    python tools/export.cli session_a session_b --out D:\\datasets --include-lossy

One output directory per session (``<out>/<session_id>/``): per-source CSVs
with #META schema rows on top (D15, data rows verbatim) plus
``export_manifest.json``. The whole invocation is validate-then-write: every
session is read and validated (and the lossy gate applied) before the first
output file exists, so a failure never leaves a half-exported dataset behind.

Lossy sessions (``raw_drops > 0`` or ``malformed_rows > 0``) are refused
with exit 2 by default — the same exit-code map as
``tools/replay/compare.py``; ``--include-lossy`` lifts the gate and the
counters still go into ``#META`` (D17: loss is recorded, never silent,
never repaired).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

if __package__ in (None, ""):  # allow `python tools/export/cli.py ...` from anywhere
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from export.csv_export import ExportResult, write_session_export  # noqa: E402
from export.session import Session, SessionError, load_session  # noqa: E402

# Exit-code map, identical to tools/replay/compare.py: 0 ok, 2 lossy
# recording refused, 4 missing session/unknown schema/unreadable input.
# 3 stays compare-only (field mismatch) — export never claims a comparison.
# [DERIVED] spec §16 constants table.
EXIT_OK = 0
EXIT_REFUSED_LOSSY = 2
EXIT_BAD_SESSION = 4

# D17 default gate: refuse lossy sessions unless --include-lossy is given.
# [DERIVED] spec §16 constants table (mirrors compare.py exit 2).
DEFAULT_INCLUDE_LOSSY = False


def _loss_counters(meta: dict[str, object]) -> dict[str, int]:
    """The counters that make a recording lossy (compare.py's exact gate)."""
    keys = ("raw_drops", "imu_drops", "ppg_drops", "line_drops", "malformed_rows")
    return {key: int(meta.get(key) or 0) for key in keys}


def _is_lossy(meta: dict[str, object]) -> bool:
    counters = _loss_counters(meta)
    return counters["raw_drops"] > 0 or counters["malformed_rows"] > 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Export recorded bracelet sessions into schema-annotated CSVs."
    )
    parser.add_argument(
        "sessions", nargs="+", metavar="SESSION_DIR", help="recorded session directory"
    )
    parser.add_argument("--out", required=True, help="directory that receives exports")
    parser.add_argument(
        "--include-lossy",
        action="store_true",
        default=DEFAULT_INCLUDE_LOSSY,
        help="export lossy sessions anyway (counters still recorded in #META)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    out_root = Path(args.out)
    if out_root.exists() and any(out_root.iterdir()):
        print(f"error: --out target {out_root} exists and is not empty", file=sys.stderr)
        return EXIT_BAD_SESSION

    # Phase 1 — validate every session (schema, columns, truncated rows)
    # before any output exists (validate-then-write).
    sessions: list[Session] = []
    for session_dir in args.sessions:
        try:
            sessions.append(load_session(session_dir))
        except SessionError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return EXIT_BAD_SESSION

    # Phase 2 — lossy gate (D17): refused entirely, nothing written.
    if not args.include_lossy:
        for session in sessions:
            if _is_lossy(session.metadata):
                counters = _loss_counters(session.metadata)
                print(
                    f"refusing lossy session {session.session_id}: "
                    f"raw_drops={counters['raw_drops']} "
                    f"(imu={counters['imu_drops']} ppg={counters['ppg_drops']} "
                    f"line={counters['line_drops']}) "
                    f"malformed_rows={counters['malformed_rows']} — rerun with "
                    "--include-lossy to export anyway (counters are recorded in #META)",
                    file=sys.stderr,
                )
                return EXIT_REFUSED_LOSSY

    # Phase 3 — write.
    for session in sessions:
        try:
            result = write_session_export(session, out_root)
        except OSError as exc:
            print(
                f"error: cannot write export for {session.session_id}: {exc}",
                file=sys.stderr,
            )
            return EXIT_BAD_SESSION
        _print_summary(session.metadata, result)
    return EXIT_OK


def _print_summary(meta: dict[str, object], result: ExportResult) -> None:
    manifest_path = result.out_dir / "export_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    print(f"session: {result.session_id}")
    print(
        f"files: {', '.join(result.exported)}"
        + (
            f" | absent (never fabricated): {', '.join(result.absent)}"
            if result.absent
            else ""
        )
    )
    events = result.out_dir / "events.csv"
    if events.exists():
        rows = [
            line
            for line in events.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        print(f"events: {max(0, len(rows) - 1)} rows")  # minus the header line
    if _is_lossy(meta):
        counters = _loss_counters(meta)
        print(
            "lossy: yes (recorded in #META — raw_drops="
            f"{counters['raw_drops']}, malformed_rows={counters['malformed_rows']})"
        )
    print(
        f"manifest: {manifest_path} "
        f"(export_schema_version={manifest['export_schema_version']})"
    )


if __name__ == "__main__":
    raise SystemExit(main())
