"""Shared pytest fixtures (spec §9).

Registers the source-layout package mapping `companion` -> ../lib (the same mapping
pyproject's package-dir applies at install time) so the suite runs from source with
`python -m pytest tests` and no install step.
"""

from __future__ import annotations

import asyncio
import pathlib
import sys
import types
from collections.abc import Coroutine
from typing import Any, TypeVar

import pytest

_LIB = pathlib.Path(__file__).resolve().parents[1] / "lib"
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by tests)"
    sys.modules["companion"] = _pkg

from companion.data import db  # noqa: E402
from companion.data.event_repository import EventRepository  # noqa: E402

_R = TypeVar("_R")


class FakeClock:
    """Deterministic monotonic clock for view-models — no wall time in tests."""

    def __init__(self, start_ms: int = 1_000_000) -> None:
        self.t = start_ms

    def now_ms(self) -> int:
        return self.t

    def advance(self, ms: int) -> None:
        self.t += ms


def run(coro: Coroutine[Any, Any, _R]) -> _R:
    """Drive any async seam with the stdlib runner — no pytest-asyncio (spec §7)."""
    return asyncio.run(coro)


@pytest.fixture
def clock() -> FakeClock:
    return FakeClock()


@pytest.fixture
def db_conn(tmp_path: pathlib.Path):
    conn = db.open_db(tmp_path / "companion.sqlite3")
    yield conn
    conn.close()


@pytest.fixture
def repo(db_conn) -> EventRepository:
    return EventRepository(db_conn)
