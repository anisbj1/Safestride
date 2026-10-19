"""F16-AUD-1 regression guard - the whole-package invocation must stay clean.

Before the fix, `history/tests/conftest.py` registered a *second* top-level
module named `conftest`, which collided with F21's `from conftest import run`
files: `python -m pytest companion` failed with 4 collection errors while the
per-suite invocations stayed green, so CI never saw it. This test re-runs
collection as a subprocess, so the breakage can never pass silently again
(reverting-fail demonstrated: deleting `__init__.py` from this package makes
this test fail; restoring it turns it green).
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

# [DERIVED] Exact expected count at this branch tip: F21 94 + F16 26 + this
# smoke test 1 = 121. Suites not on this branch (F20/F22) may add more later,
# hence >=; what must never happen again is a missing suite or a collection
# error, both asserted below.
_MIN_EXPECTED_COLLECTED = 121

# [DERIVED] Generous subprocess ceiling; whole-package collection takes <1 s.
_COLLECT_TIMEOUT_S = 120

# Worktree root: this file lives at <root>/companion/lib/features/history/tests/.
_ROOT = pathlib.Path(__file__).resolve().parents[5]


def test_whole_package_collection_is_clean() -> None:
    """`python -m pytest companion` collects every suite with zero errors."""
    proc = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "companion",
            "--collect-only",
            "-q",
            "-p",
            "no:cacheprovider",
        ],
        capture_output=True,
        text=True,
        cwd=_ROOT,
        timeout=_COLLECT_TIMEOUT_S,
    )
    out = proc.stdout + proc.stderr
    node_ids = [line for line in out.splitlines() if "::" in line]
    # pytest exits 2 on collection errors; assert explicitly for a readable message.
    assert proc.returncode == 0, f"collection failed (rc={proc.returncode}):\n{out}"
    assert "errors during collection" not in out, out
    assert len(node_ids) >= _MIN_EXPECTED_COLLECTED, out
    # Both suites must be present, so a silently missing suite also fails.
    assert any(nid.startswith("tests/test_") for nid in node_ids), out  # F21 suite
    assert any(nid.startswith("lib/features/history/tests/test_") for nid in node_ids), (
        out  # F16 suite
    )
