"""Make the ml/eval modules and the test fixtures importable (ml/common convention).

``ml/eval`` is not a Python package (``ml/pyproject.toml`` is F00-owned and
declares no packages), so tests import ``metrics`` / ``event_matching`` /
``split`` / ``report`` / ``run_eval`` as top-level modules and
``fixtures.synthetic`` as a namespace package under ``ml/eval/tests``. The
path fix lives here so the test modules keep clean, sorted imports.
"""

import sys
from pathlib import Path

import pytest

_TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TESTS_DIR / "fixtures"))  # tests/fixtures → synthetic
sys.path.insert(0, str(_TESTS_DIR))  # ml/eval/tests → fixtures.*
sys.path.insert(0, str(_TESTS_DIR.parent))  # ml/eval → metrics, ...


@pytest.fixture(scope="session")
def canonical_sessions(tmp_path_factory: pytest.TempPathFactory) -> tuple[list[Path], str]:
    """The shared hand-computed two-session set (spec §9 e2e), built once."""
    import synthetic  # on sys.path via the tests/fixtures insert above

    root = tmp_path_factory.mktemp("canonical_sessions")
    return synthetic.write_canonical_sessions(root)
