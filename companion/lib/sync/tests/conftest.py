"""Pytest bootstrap for the colocated F20 sync suite (Owns: companion/lib/sync/tests).

Same source-layout package mapping as F21's `companion/tests/conftest.py`
(`companion` -> `../lib`), computed for this directory's depth. This directory
is a package (`__init__.py`, audit F20-AUD-3 / same defect as F16-AUD-1), so
pytest imports this file as `lib.sync.tests.conftest` and it never registers a
top-level module named `conftest` that could shadow F21's. Helpers (FakeClock,
`run`, builders) live in `.sync_test_support` and are imported package-
relatively, so test modules never import from a `conftest` module name.
"""

from __future__ import annotations

import pathlib
import sys
import types

import pytest

_LIB = pathlib.Path(__file__).resolve().parents[2]  # companion/lib
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by tests)"
    sys.modules["companion"] = _pkg

from .sync_test_support import FakeClock  # noqa: E402  (after the package mapping)


@pytest.fixture
def clock() -> FakeClock:
    return FakeClock()
