"""Make the `export` package importable no matter where pytest is started.

Mirrors tools/capture/tests/conftest.py (same sys.path mechanism, so
`import decode_events` and `import export` resolve identically).
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parents[2]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

FIXTURE_SESSION = Path(__file__).resolve().parent / "fixtures" / "session_full"


@pytest.fixture
def fixture_session(tmp_path) -> Path:  # noqa: ANN001 - pytest fixture
    """A private copy of the committed fixture session (never mutate the original)."""
    target = tmp_path / "session_full"
    shutil.copytree(FIXTURE_SESSION, target)
    return target


@pytest.fixture
def out_dir(tmp_path) -> Path:  # noqa: ANN001 - pytest fixture
    path = tmp_path / "export_out"
    path.mkdir()
    return path
