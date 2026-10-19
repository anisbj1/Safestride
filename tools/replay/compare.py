"""F28 host replay comparison (spec §9.6): rerun bw_signal on a recorded
session's raw rows and diff the regenerated feature rows against the
session's recorded features.csv.

Exit codes (plan §15 / D9 / D12):

  0  match within tolerance (values accepted "consistent within recording
     precision" and rows reported "not comparable"/"lost in recording" are
     printed and counted — never silent)
  2  comparison INVALID: the session records raw drops or malformed rows, so
     inputs are known-lossy — a diff would be meaningless and this must never
     be reported as a pass
  3  mismatch: a comparable feature row differs beyond tolerance AND beyond
     the recording-precision envelope, or is missing/extra without evidence
     that the recording lost it
  4  usage: session directory, fixture or replay binary not found

Consistency model (D12, 2026-10-13): the DEVICE computes features from
full-precision float32 samples, but the RECORDING stores every I-row sample
rounded to the print format (%.4f accel = 1e-4 g ≈ 3.3 MPU6050 LSBs,
%.1f gyro). A replay therefore recomputes from inputs that differ from the
device's true inputs by up to half a print quantum per sample, and a feature
recomputed from rounded inputs can move beyond the base tolerance even when
the pipeline is bit-identical (replay vs replay: 598/598 identical on the
bench capture). Measurement that set the envelope below: K=5 re-runs with
inputs jittered inside their print bins reproduced the observed
|device − replay| drift (jerk p50 2.0e-4 / p90 6.0e-4 vs 2.0e-4 / 6.0e-4;
share beyond 2.5e-4: 45.9 % vs 45.6 %), and the max reachable per-field
spread covers the max device drift (jerk 1.2e-3 vs 1.0e-3, dominant_freq
0.562 vs 0.532). A value beyond TOLERANCE_REL/TOLERANCE_ABS is therefore
accepted only within ENVELOPE_SPREAD scaled by the session's actual input
quantum (a future %.6f capture is verified ~100× tighter with no code
change); anything further is a real mismatch. TOLERANCE_* is never widened.

Rows whose window inputs are not in the recording (capture attached
mid-stream, or UART tearing removed samples inside the window) cannot be
recomputed at all: they are counted, named and excluded — never diffed,
never silently passed (same honesty rule as raw_drops => exit 2).

Usage:
    python tools/replay/compare.py <session_dir> [--binary PATH]
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from capture import lines as linemod  # noqa: E402 - after the path shim above

# [EXPERIMENTAL] F28 D9: x86-vs-xtensa libm differences plus print rounding —
# the fixture round-trip showed no drift beyond this. Never widened: D12
# drift is handled by the recording-precision envelope below, not by
# loosening this bound.
TOLERANCE_REL = 1e-3
TOLERANCE_ABS = 1e-4

# [EXPERIMENTAL] D12 recording-precision envelope: max reachable feature
# drift when the replay recomputes from I rows rounded to ENVELOPE_QUANTUM_REF
# (measured 2026-10-13 bench capture, K=5 print-bin perturbation re-runs —
# see the module docstring; every value also covers the measured device
# drift with ≥0 headroom, ENVELOPE_HEADROOM adds the safety margin).
# Scales LINEARLY with the session's measured input quantum, so higher-
# precision recordings are verified proportionally tighter with no code
# change. Fields absent from the table (and ppg fields, whose integer-count
# inputs are lossless) have zero envelope: base tolerance only.
ENVELOPE_QUANTUM_REF = 1e-4
ENVELOPE_SPREAD = {
    "enmo_g": 1e-4,
    "mag_std_g": 1e-4,
    "jerk_rms_g_per_s": 1.2e-3,
    "gyro_rms_dps": 0.0,
    "gravity_tilt_deg": 0.0,
    "dominant_freq_hz": 0.562,  # thresholded detection flips 0 <-> ~0.5 Hz
    "stillness_ms": 0.0,
}
ENVELOPE_HEADROOM = 2.0

# [DERIVED] Mirror of bw::signal kMotionWindowMs (the host tool cannot
# include the firmware header): the input span a feature row depends on is
# (t_ms - kMotionWindowMs, t_ms].
MOTION_WINDOW_MS = 1000

# Row kinds the M1 replay can regenerate (context/activity classification is
# F12 and is not replayed in M1 — those fixture rows are reported and skipped,
# never compared, never silently counted as a pass).
KIND_TO_CSV = {"motion": "motion", "ppg_features": "ppg"}

MOTION_FIELDS = [
    "quality",
    "enmo_g",
    "mag_std_g",
    "jerk_rms_g_per_s",
    "gyro_rms_dps",
    "gravity_tilt_deg",
    "dominant_freq_hz",
    "stillness_ms",
]
PPG_FIELDS = [
    "quality",
    "hr_valid",
    "hr_bpm",
    "hr_confidence",
    "perfusion_index",
    "ir_dc_level",
    "sqi",
    "motion_corrupted",
    "hrv_valid",
    "rmssd_ms",
]
FIELDS_BY_CSV_KIND = {"motion": MOTION_FIELDS, "ppg": PPG_FIELDS}

MAX_REPORTED_DIFFS = 10

# A torn line in diagnostics.log: the tail of a research row that an
# un-gated diagnostic print interleaved into (garbage prefix + intact
# suffix, e.g. "<60 garbage bytes>0,Insufficient,..." or
# "<garbage>I,105656,0.0852,..."). Its presence proves the recording lost
# whole research rows that the device did emit — used ONLY to attribute rows
# replayable from the raw inputs but absent from features.csv (F28 D12).
_TORN_FRAGMENT = re.compile(
    r"I,[0-9]{3,},"                                  # intact I-row body after garbage
    r"|[0-9],(?:F,M|F,P),[0-9]{3,},"                 # intact F-row start after garbage
    r"|[0-9],(?:Good|Degraded|Insufficient),"        # F-row tail (quality field)
)


def _default_binary() -> Path:
    name = "program.exe" if sys.platform == "win32" else "program"
    return TOOLS_DIR / "replay" / ".pio" / "build" / "native" / name


def _within_tolerance(expected: str, actual: object) -> bool:
    if isinstance(actual, bool):  # parsed replay rows carry real bools; the
        actual = 1 if actual else 0  # fixture CSV stores them as "0"/"1"
    # DATASET_FORMAT.md §5 writes feature booleans as 0/1, but sessions
    # captured before 2026-10-13 wrote Python's str(bool) ("True"/"False") —
    # both spellings compare by VALUE, never by text.
    if expected in ("True", "False"):
        expected = "1" if expected == "True" else "0"
    try:
        expected_value = float(expected)
        actual_value = float(actual)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return expected == str(actual)  # enums compare exactly
    diff = abs(expected_value - float(actual_value))
    return diff <= max(TOLERANCE_ABS, TOLERANCE_REL * abs(expected_value))


def _load_fixture(
    session_dir: Path,
) -> tuple[dict[tuple[str, str], dict[str, str]], dict[str, int]]:
    fixture = session_dir / "features.csv"
    if not fixture.exists():
        raise FileNotFoundError(f"fixture not found: {fixture}")
    by_key: dict[tuple[str, str], dict[str, str]] = {}
    skipped: dict[str, int] = {}
    with fixture.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            kind = row.get("kind", "")
            if kind not in FIELDS_BY_CSV_KIND:
                skipped[kind] = skipped.get(kind, 0) + 1
                continue
            by_key[(kind, row["t_ms"])] = row
    return by_key, skipped


def _run_replay(binary: Path, session_dir: Path) -> list[str]:
    result = subprocess.run(
        [str(binary), str(session_dir)], capture_output=True, text=True, check=False
    )
    if result.returncode == 4:
        raise SystemExit(4)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(3)
    return result.stdout.splitlines()


def _load_replay_rows(output_lines: list[str]) -> dict[tuple[str, str], dict[str, object]]:
    replayed: dict[tuple[str, str], dict[str, object]] = {}
    for line in output_lines:
        if not linemod.is_research_line(line):
            continue  # the runner may print notes; only F rows are data
        try:
            kind, row = linemod.parse_line(line)
        except (linemod.MalformedRow, linemod.SchemaMismatch) as exc:
            print(f"replay output not parseable: {exc}", file=sys.stderr)
            raise SystemExit(3) from exc
        csv_kind = KIND_TO_CSV.get(kind)
        if csv_kind is None:
            continue
        replayed[(csv_kind, str(row["t_ms"]))] = row
    return replayed


class _RawInputs:
    """The recorded raw stream: timestamps, cadence and print quantum."""

    def __init__(self, path: Path, period_hint: int | None = None) -> None:
        self.path = path
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.reader(handle)
            self.header = next(reader)
            self.rows = [row for row in reader if len(row) == len(self.header)]
        self.times = [int(float(row[0])) for row in self.rows]
        # The device prints fixed-point (printf %.*f), so the number of
        # decimals in the text IS the print quantum: the true sample value
        # lies within half a quantum of what was recorded. The largest
        # column (accel, %.4f) governs the envelope scaling.
        self.quantum = self._quantum()
        self.period = self.period_ms(period_hint)

    def _quantum(self) -> float:
        decimals = 0
        for row in self.rows[:500]:
            for value in row[2:]:
                if "." in value:
                    decimals = max(decimals, len(value.split(".")[1]))
        return 10.0 ** -decimals

    def period_ms(self, hint: int | None) -> int:
        if hint is not None and hint > 0:
            return int(hint)
        deltas = [b - a for a, b in zip(self.times, self.times[1:], strict=False) if b > a]
        if not deltas:
            return 0
        return max(set(deltas), key=deltas.count)  # modal delta (session.py rule)


class _Coverage:
    """Window-input coverage of one raw stream (D12): a feature row is only
    recomputable when every expected sample slot of its window was recorded.
    Slot arithmetic mirrors session.py::_gap_stats (DATASET_FORMAT §3): the
    modal delta is the period and a delta of k periods hides k-1 slots — the
    2026-10-13 capture showed pure ±1 ms cadence jitter (19/20/21 ms), so
    round() never confuses jitter with loss."""

    def __init__(self, times: list[int], period: int) -> None:
        self.times = times
        self.period = period
        self.gaps: list[tuple[int, int]] = []
        if times and period > 0:
            for a, b in zip(times, times[1:], strict=False):
                if round((b - a) / period) - 1 > 0:
                    self.gaps.append((a + period, b - period))

    def missing(self, t_ms: int, window_ms: int) -> tuple[str, str] | None:
        """(category, detail) why the window ending at t_ms cannot be
        recomputed, or None when every expected input slot was recorded."""
        if not self.times or self.period <= 0:
            return None  # no coverage information: compared strictly
        window_start = t_ms - window_ms
        if window_start < self.times[0]:
            hidden = round((self.times[0] - window_start) / self.period)
            if hidden > 0:
                return ("window starts before the recording",
                        f"{hidden} sample slots before the first recorded row")
        if window_start >= self.times[-1]:
            return ("window starts after the recording", "no recorded rows in window")
        for gap_start, gap_end in self.gaps:
            if gap_start <= t_ms and gap_end > window_start:
                return ("window overlaps recorded sample loss",
                        f"loss at t_ms {gap_start}..{gap_end}")
        return None


def _load_inputs(
    session_dir: Path, file_name: str, period_key: str, meta: dict[str, object]
) -> _RawInputs | None:
    path = session_dir / file_name
    if not path.exists():
        return None
    hint = meta.get(period_key)
    return _RawInputs(path, int(hint) if isinstance(hint, int) else None)


def _torn_research_fragments(session_dir: Path) -> int:
    """Count diagnostics.log lines that carry the remains of a research row
    (garbage prefix + row suffix). Proof that the recording lost rows the
    device emitted — see _TORN_FRAGMENT."""
    path = session_dir / "diagnostics.log"
    if not path.exists():
        return 0
    text = path.read_bytes().decode("utf-8", errors="replace")
    return sum(1 for line in text.splitlines() if _TORN_FRAGMENT.search(line))


def _envelope_width(field: str, quantum: float) -> float:
    spread = ENVELOPE_SPREAD.get(field, 0.0)
    return spread * ENVELOPE_HEADROOM * (quantum / ENVELOPE_QUANTUM_REF)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compare a host replay with a recording.")
    parser.add_argument("session", help="session directory (raw_*.csv + features.csv)")
    parser.add_argument(
        "--binary", type=Path, default=_default_binary(), help="replay binary"
    )
    args = parser.parse_args(argv)

    session_dir = Path(args.session)
    session_json = session_dir / "session.json"
    if not session_json.exists():
        print(f"usage: session.json not found in {session_dir}", file=sys.stderr)
        return 4
    if not Path(args.binary).exists():
        print(
            f"usage: replay binary not found at {args.binary} — run "
            "`pio run -d tools/replay` first",
            file=sys.stderr,
        )
        return 4

    meta = json.loads(session_json.read_text(encoding="utf-8"))
    if meta.get("raw_drops", 0) > 0:
        print(
            f"comparison invalid: session recorded raw_drops={meta['raw_drops']} "
            "(missing input rows make any diff meaningless)",
            file=sys.stderr,
        )
        return 2
    if meta.get("malformed_rows", 0) > 0:
        print(
            f"comparison invalid: session recorded "
            f"malformed_rows={meta['malformed_rows']}",
            file=sys.stderr,
        )
        return 2

    try:
        fixture, skipped_kinds = _load_fixture(session_dir)
    except FileNotFoundError as exc:
        print(f"usage: {exc}", file=sys.stderr)
        return 4
    for kind, count in sorted(skipped_kinds.items()):
        print(f"note: {count} fixture rows of kind {kind!r} are not replayed in M1, skipped")

    binary = Path(args.binary)
    replayed = _load_replay_rows(_run_replay(binary, session_dir))

    # Window-input coverage: motion rows need the recorded IMU stream; PPG
    # rows are computed from integer counts (lossless) and get the same check
    # when raw_ppg.csv exists.
    imu = _load_inputs(session_dir, "raw_imu.csv", "imu_period_ms", meta)
    ppg = _load_inputs(session_dir, "raw_ppg.csv", "ppg_period_ms", meta)
    coverages = {
        "motion": _Coverage(imu.times, imu.period) if imu else None,
        "ppg": _Coverage(ppg.times, ppg.period) if ppg else None,
    }
    quantum = imu.quantum if imu is not None else ENVELOPE_QUANTUM_REF

    mismatches: list[str] = []
    not_comparable: dict[str, dict[str, object]] = {}
    pending: list[tuple[tuple[str, str], str, str, object]] = []
    compared_rows = 0

    for key, expected_row in sorted(fixture.items()):
        csv_kind, t_ms = key
        coverage = coverages.get(csv_kind)
        if coverage is not None:
            uncovered = coverage.missing(int(t_ms), MOTION_WINDOW_MS)
            if uncovered is not None:
                category, detail = uncovered
                entry = not_comparable.setdefault(category, {"detail": detail, "rows": []})
                entry["rows"].append(f"{csv_kind} t_ms={t_ms}")
                continue
        actual_row = replayed.get(key)
        if actual_row is None:
            mismatches.append(f"missing {csv_kind} row at t_ms={t_ms}")
            continue
        compared_rows += 1
        for field in FIELDS_BY_CSV_KIND[csv_kind]:
            expected = str(expected_row.get(field, ""))
            actual = actual_row.get(field, "")
            if _within_tolerance(expected, actual):
                continue
            try:
                float(expected)
                float(actual)  # type: ignore[arg-type]
            except (TypeError, ValueError):
                mismatches.append(f"{csv_kind} t_ms={t_ms} {field}: {expected} != {actual}")
            else:
                pending.append((key, field, expected, actual))

    # Replay rows the recording lacks: the device emitted them (replay can
    # only regenerate what the recorded inputs produce), so either the
    # recording lost them to tearing or the device skipped them. Torn
    # research fragments in diagnostics.log are direct evidence of the
    # former; without such evidence an extra row is a real mismatch.
    extra = [key for key in sorted(replayed) if key not in fixture]
    torn_fragments = _torn_research_fragments(session_dir)
    lost_rows: list[tuple[str, str]] = []
    if extra and torn_fragments:
        lost_rows = extra
    else:
        mismatches.extend(f"extra {key[0]} row at t_ms={key[1]}" for key in extra)

    # Values beyond the base tolerance: accepted only inside the recording-
    # precision envelope (D12); anything further is a real mismatch.
    consistent = 0
    for key, field, expected, actual in pending:
        try:
            expected_value = float(expected)
            actual_value = float(actual)  # type: ignore[arg-type]
        except (TypeError, ValueError):  # pragma: no cover - guarded above
            mismatches.append(f"{key[0]} t_ms={key[1]} {field}: {expected} != {actual}")
            continue
        diff = abs(expected_value - actual_value)
        tolerance = max(TOLERANCE_ABS, TOLERANCE_REL * abs(expected_value))
        width = _envelope_width(field, quantum)
        if diff <= tolerance + width:
            consistent += 1
            if consistent <= MAX_REPORTED_DIFFS:
                print(
                    f"CONSISTENT {key[0]} t_ms={key[1]} {field}: {expected} != {actual} "
                    f"(|diff|={diff:.4g} within recording-precision envelope "
                    f"{width:.4g} + tolerance {tolerance:.3g})"
                )
        else:
            mismatches.append(f"{key[0]} t_ms={key[1]} {field}: {expected} != {actual}")

    skipped_rows = sum(len(entry["rows"]) for entry in not_comparable.values())
    for category, entry in sorted(not_comparable.items()):
        rows = entry["rows"]
        print(
            f"not comparable: {len(rows)} rows [{category}] "
            f"({', '.join(rows[:5])}{' ...' if len(rows) > 5 else ''}) — "
            f"{entry['detail']}"
        )
    if consistent > MAX_REPORTED_DIFFS:
        print(
            f"... and {consistent - MAX_REPORTED_DIFFS} more values consistent "
            "within recording precision"
        )
    if lost_rows:
        print(
            f"lost in recording: {len(lost_rows)} rows in replay but not in "
            f"features.csv ({', '.join(f'{k[0]} t_ms={k[1]}' for k in lost_rows[:5])}"
            f"{' ...' if len(lost_rows) > 5 else ''}) — {torn_fragments} torn research "
            "fragments in diagnostics.log prove recording-side row loss"
        )

    if mismatches:
        for diff in mismatches[:MAX_REPORTED_DIFFS]:
            print(f"MISMATCH {diff}", file=sys.stderr)
        if len(mismatches) > MAX_REPORTED_DIFFS:
            print(f"... and {len(mismatches) - MAX_REPORTED_DIFFS} more", file=sys.stderr)
        return 3

    envelope_note = ""
    if consistent:
        envelope_note = (
            f" | {consistent} values consistent within recording precision "
            f"(input quantum {quantum:g}, envelope {ENVELOPE_HEADROOM:g}x measured)"
        )
    skipped_note = (
        f" | {skipped_rows} rows not comparable (window inputs not recorded)"
        if skipped_rows
        else ""
    )
    lost_note = f" | {len(lost_rows)} rows lost in recording (torn lines)" if lost_rows else ""
    print(
        f"match: {compared_rows} rows within rel={TOLERANCE_REL} abs={TOLERANCE_ABS}"
        f"{envelope_note}{skipped_note}{lost_note}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
