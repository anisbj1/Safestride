"""Regenerates tools/replay/fixtures/session_basic — the F28 replay fixture.

PROVENANCE: fully synthetic signal (deterministic sinusoid IMU + pulsed PPG),
no recorded person data, no patient data. The raw rows are written first, then
the replay binary regenerates features.csv from them, so compare.py returns 0
against this fixture by construction — a CHARACTERIZATION pin. It exists to
catch accidental pipeline changes: a future bw_signal change that moves these
numbers beyond tolerance must fail CI until someone re-runs this script
deliberately and reviews the diff.

Run ONLY intentionally:

    python tools/replay/make_fixture.py          # after `pio run -d tools/replay`
"""

from __future__ import annotations

import csv
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from capture.session import FEATURES_COLUMNS  # noqa: E402 - after the path shim

FIXTURE_DIR = TOOLS_DIR / "replay" / "fixtures" / "session_basic"

# [DERIVED] Fixture shape: 12 s gives 11 full 1 s windows (F02 window) of each
# kind — enough for drift to show while staying reviewable line-by-line.
DURATION_MS = 12000
IMU_PERIOD_MS = 20  # [HW] 50 Hz (F01 D15)
PPG_PERIOD_MS = 10  # [HW] 100 Hz (F01 D15)
BOOT_ID = 7
FIRMWARE_VERSION = "0.1.0"

# [EXPERIMENTAL] Motion amplitude: a gentle 1.2 Hz sway on x (0.05 g) over
# gravity — enough for ENMO to be nonzero in every window without saturating.
MOTION_AMPLITUDE_G = 0.05
MOTION_FREQ_HZ = 1.2

# [EXPERIMENTAL] PPG: 60 bpm sharp pulses on a DC baseline — shaped so the
# beat detector has a periodic feature to lock onto.
PPG_DC = 100000
PPG_PULSE_AMPLITUDE = 30000
PPG_BPM = 60.0


def imu_rows() -> list[list[object]]:
    rows: list[list[object]] = []
    for t_ms in range(0, DURATION_MS, IMU_PERIOD_MS):
        seconds = t_ms / 1000.0
        ax = MOTION_AMPLITUDE_G * math.sin(2.0 * math.pi * MOTION_FREQ_HZ * seconds)
        rows.append([t_ms, BOOT_ID, round(ax, 4), 0.0, 1.0, 0.0, 0.0, 1.5])
    return rows


def ppg_rows() -> list[list[object]]:
    rows: list[list[object]] = []
    phase_hz = PPG_BPM / 60.0
    for t_ms in range(0, DURATION_MS, PPG_PERIOD_MS):
        seconds = t_ms / 1000.0
        # Raised-cosine pulse once per beat: periodic, zero outside the beat.
        beat = (seconds * phase_hz) % 1.0
        pulse = PPG_PULSE_AMPLITUDE * max(0.0, math.cos(2.0 * math.pi * beat)) ** 8
        red = int(PPG_DC + pulse)
        rows.append([t_ms, BOOT_ID, red, int(PPG_DC * 2 + pulse * 2)])
    return rows


def write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(rows)


def write_session_json() -> None:
    session = {
        "schema_version": 1,
        "session_id": "research_fixture0000T000000Z",
        "boot_id": BOOT_ID,
        "firmware_version": FIRMWARE_VERSION,
        "started_utc": "2026-01-01T00:00:00Z",
        "ended_utc": "2026-01-01T00:00:12Z",
        "end_reason": "user",
        "row_counts": {"header": 1},
        "raw_drops": 0,
        "line_drops": 0,
        "malformed_rows": 0,
        "diagnostics_lines": 0,
        "raw_tee": True,
        "counters_as_of_hdr_seconds": 1,
    }
    (FIXTURE_DIR / "session.json").write_text(
        json.dumps(session, indent=2) + "\n", encoding="utf-8"
    )


def replay_binary() -> Path:
    name = "program.exe" if sys.platform == "win32" else "program"
    binary = TOOLS_DIR / "replay" / ".pio" / "build" / "native" / name
    if binary.exists():
        return binary
    pio = shutil.which("pio") or str(Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe")
    result = subprocess.run(
        [pio, "run", "-d", str(TOOLS_DIR / "replay")], capture_output=True, text=True, check=False
    )
    if result.returncode != 0 or not binary.exists():
        sys.exit(f"could not build the replay binary:\n{result.stdout}\n{result.stderr}")
    return binary


def main() -> int:
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    write_csv(
        FIXTURE_DIR / "raw_imu.csv",
        ["t_ms", "boot_id", "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps"],
        imu_rows(),
    )
    write_csv(
        FIXTURE_DIR / "raw_ppg.csv", ["t_ms", "boot_id", "red", "ir"], ppg_rows()
    )
    write_session_json()

    result = subprocess.run(
        [str(replay_binary()), str(FIXTURE_DIR)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        sys.exit(f"replay failed ({result.returncode}): {result.stderr}")

    # stdout holds F,M / F,P rows; reshape them into the session features.csv.
    rows: list[dict[str, object]] = []
    for line in result.stdout.splitlines():
        if line.startswith("F,M,"):
            fields = line.split(",")
            rows.append(_motion_row(fields))
        elif line.startswith("F,P,"):
            fields = line.split(",")
            rows.append(_ppg_row(fields))

    with (FIXTURE_DIR / "features.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FEATURES_COLUMNS, restval="")
        writer.writeheader()
        writer.writerows(rows)

    print(f"fixture written: {FIXTURE_DIR} ({len(rows)} feature rows)")
    return 0


def _motion_row(fields: list[str]) -> dict[str, object]:
    return {
        "t_ms": fields[2],
        "boot_id": BOOT_ID,
        "kind": "motion",
        "quality": fields[3],
        "enmo_g": fields[4],
        "mag_std_g": fields[5],
        "jerk_rms_g_per_s": fields[6],
        "gyro_rms_dps": fields[7],
        "gravity_tilt_deg": fields[8],
        "dominant_freq_hz": fields[9],
        "stillness_ms": fields[10],
    }


def _ppg_row(fields: list[str]) -> dict[str, object]:
    return {
        "t_ms": fields[2],
        "boot_id": BOOT_ID,
        "kind": "ppg",
        "quality": fields[3],
        "hr_valid": fields[4],
        "hr_bpm": fields[5],
        "hr_confidence": fields[6],
        "perfusion_index": fields[7],
        "ir_dc_level": fields[8],
        "sqi": fields[9],
        "motion_corrupted": fields[10],
        "hrv_valid": fields[11],
        "rmssd_ms": fields[12],
    }


if __name__ == "__main__":
    raise SystemExit(main())
