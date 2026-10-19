#!/usr/bin/env python3
"""CI entry point for the anarolepsy bracelet repository. [F00 D11]

Runs F00's spec §3 checks in D11's order, extended twice:
  1. pio test -e native       host unit tests (Unity, pio test -e native)
  2. pio run -e esp32dev      device firmware builds (also compiles bw/contract_checks.h)
  3. ruff check               Python hygiene for ml/, tools/, ci/, companion/
  4. clang-format --dry-run   C++ format gate over firmware/lib|src|test
  5. pytest companion         companion host tests (W5 — the app scaffold + halves)
  6. pytest tools             tools/ suite (queue 12: replay-fixture drift was
                              invisible here — ruff ran on tools/ but its pytest
                              did not; gated now that the fixture is re-captured)

Design (D11): one file, subprocess only, no framework; cross-platform (the owner
is on Windows now, a hosted CI later); per-step skip flags; aggregated pass/fail;
non-zero exit; a printed summary naming exactly which step failed.

Usage:
    python ci/check.py [--skip-native] [--skip-esp32] [--skip-ruff] [--skip-format]
                       [--skip-companion] [--skip-tools]

Exit codes: 0 = every executed step passed · 1 = at least one step failed ·
2 = bad usage (argparse).

Tool resolution (§17.5 environment, measured — do not rediscover): pio, ruff,
pytest and clang-format are NOT on PATH in this environment, so each tool has an
explicit fallback chain below instead of assuming PATH.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_DIR = REPO_ROOT / "firmware"
COMPANION_DIR = REPO_ROOT / "companion"
TOOLS_DIR = REPO_ROOT / "tools"

# Format-gate scope (status log, known non-blocker): firmware/lib|src|test only.
# firmware/include/config/hw_config.h is F01-owned and not format-clean yet —
# exclude it, never edit it from F00.
FORMAT_PATHS = ["firmware/lib", "firmware/src", "firmware/test"]
FORMAT_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".ino"}

# Ruff runs over every first-party Python tree that exists today.
RUFF_PATHS = ["ci", "ml", "tools", "companion"]


def _find_pio():
    """pio is not on PATH here; fall back to the PlatformIO venv (measured path)."""
    on_path = shutil.which("pio") or shutil.which("pio.exe")
    if on_path:
        return [on_path]
    venv_scripts = Path.home() / ".platformio" / "penv" / "Scripts"
    for name in ("pio.exe", "pio") if os.name == "nt" else ("pio",):
        candidate = venv_scripts / name
        if candidate.exists():
            return [str(candidate)]
    return None


def _find_ruff():
    """Prefer the running interpreter's module; fall back to its Scripts dir."""
    probe = subprocess.run(
        [sys.executable, "-m", "ruff", "--version"],
        capture_output=True,
        check=False,
    )
    if probe.returncode == 0:
        return [sys.executable, "-m", "ruff"]
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidate = Path(appdata) / "Python" / "Python314" / "Scripts" / "ruff.exe"
        if candidate.exists():
            return [str(candidate)]
    on_path = shutil.which("ruff")
    return [on_path] if on_path else None


def _find_clang_format():
    """clang-format is a pip Scripts binary here, not importable as a module."""
    on_path = shutil.which("clang-format")
    if on_path:
        return [on_path]
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidate = Path(appdata) / "Python" / "Python314" / "Scripts"
        for name in ("clang-format.exe", "clang-format") if os.name == "nt" else (
            "clang-format",
        ):
            full = candidate / name
            if full.exists():
                return [str(full)]
    return None


def _format_files():
    files = []
    for rel in FORMAT_PATHS:
        base = REPO_ROOT / rel
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in FORMAT_SUFFIXES and path.is_file():
                # Relative to REPO_ROOT (step_clang_format runs with cwd=REPO_ROOT):
                # absolute paths overflow Windows' 32,767-char argv limit in long
                # worktree paths (backlog 25, observed as WinError 206).
                files.append(str(path.relative_to(REPO_ROOT)))
    return files


def step_native_tests():
    """1/6 pio test -e native — never `pio run -e native` (test_build_src, §13)."""
    pio = _find_pio()
    if pio is None:
        print("error: pio not found (not on PATH and no ~/.platformio/penv copy)")
        return 1
    return subprocess.run(
        pio + ["test", "-e", "native"], cwd=FIRMWARE_DIR, check=False
    ).returncode


def step_firmware_build():
    """2/6 pio run -e esp32dev — the device image must link."""
    pio = _find_pio()
    if pio is None:
        print("error: pio not found (not on PATH and no ~/.platformio/penv copy)")
        return 1
    return subprocess.run(
        pio + ["run", "-e", "esp32dev"], cwd=FIRMWARE_DIR, check=False
    ).returncode


def step_ruff():
    """3/6 ruff check over all first-party Python."""
    ruff = _find_ruff()
    if ruff is None:
        print("error: ruff not found (not importable, no Scripts copy, not on PATH)")
        return 1
    return subprocess.run(
        ruff + ["check"] + RUFF_PATHS, cwd=REPO_ROOT, check=False
    ).returncode


def step_clang_format():
    """4/6 clang-format --dry-run over firmware/lib|src|test (see FORMAT_PATHS)."""
    clang_format = _find_clang_format()
    if clang_format is None:
        print("error: clang-format not found (not on PATH, no Scripts copy)")
        return 1
    files = _format_files()
    if not files:
        print("error: no C++ files found under " + ", ".join(FORMAT_PATHS))
        return 1
    return subprocess.run(
        clang_format + ["--dry-run", "--Werror"] + files, cwd=REPO_ROOT, check=False
    ).returncode


def step_companion_tests():
    """5/6 python -m pytest companion — host tests, no BLE hardware.

    Covers the F21 scaffold plus the W5 half suites (companion/lib/**/tests/).
    Run with cwd=COMPANION_DIR so the conftest package mapping resolves.
    """
    return subprocess.run(
        [sys.executable, "-m", "pytest", ".", "-q"],
        cwd=COMPANION_DIR, check=False
    ).returncode


def step_tools_tests():
    """6/6 python -m pytest tools — the tools/ suite (F28 capture/replay/etc).

    Added for queue 12: the replay-fixture drift was invisible to CI because
    ruff covered tools/ but its pytest did not run here. The fixture is now
    re-captured (deliberate make_fixture.py run, diff reviewed) and this step
    keeps future pipeline drift red instead of silent.
    """
    return subprocess.run(
        [sys.executable, "-m", "pytest", "tools", "-q"],
        cwd=REPO_ROOT, check=False
    ).returncode


def main():
    parser = argparse.ArgumentParser(description="Run the F00 CI checks (D11).")
    parser.add_argument("--skip-native", action="store_true",
                        help="skip 1/6 pio test -e native")
    parser.add_argument("--skip-esp32", action="store_true",
                        help="skip 2/6 pio run -e esp32dev")
    parser.add_argument("--skip-ruff", action="store_true",
                        help="skip 3/6 ruff check")
    parser.add_argument("--skip-format", action="store_true",
                        help="skip 4/6 clang-format --dry-run")
    parser.add_argument("--skip-companion", action="store_true",
                        help="skip 5/6 python -m pytest companion")
    parser.add_argument("--skip-tools", action="store_true",
                        help="skip 6/6 python -m pytest tools")
    args = parser.parse_args()

    # (number, skip flag, label, callable) — order is D11's, do not reorder.
    steps = [
        ("1/6", args.skip_native, "pio test -e native", step_native_tests),
        ("2/6", args.skip_esp32, "pio run -e esp32dev", step_firmware_build),
        ("3/6", args.skip_ruff, "ruff check", step_ruff),
        ("4/6", args.skip_format, "clang-format --dry-run", step_clang_format),
        ("5/6", args.skip_companion, "pytest companion", step_companion_tests),
        ("6/6", args.skip_tools, "pytest tools", step_tools_tests),
    ]

    passed, failed, skipped = [], [], []
    for number, skip, label, fn in steps:
        if skip:
            print(f"\n=== [{number}] {label} — SKIPPED (flag) ===")
            skipped.append(label)
            continue
        print(f"\n=== [{number}] {label} ===")
        code = fn()
        if code == 0:
            print(f"=== [{number}] {label} — PASS ===")
            passed.append(label)
        else:
            print(f"=== [{number}] {label} — FAIL (exit {code}) ===")
            failed.append(label)

    print("\n--- CI summary ---")
    for label in passed:
        print(f"  PASS   {label}")
    for label in failed:
        print(f"  FAIL   {label}")
    for label in skipped:
        print(f"  SKIP   {label}")
    if failed:
        print(f"RESULT: FAILED ({len(failed)} step(s) failed)")
        return 1
    print(f"RESULT: OK ({len(passed)} passed, {len(skipped)} skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
