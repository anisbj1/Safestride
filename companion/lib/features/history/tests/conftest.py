"""Colocated test bootstrap (plan §15.2): the same source-layout package mapping as
F21's `companion/tests/conftest.py`, rooted one level deeper (…/features/history/tests).
The gates run the two suites in separate pytest processes, so the mapping never
collides with F21's conftest.
"""

from __future__ import annotations

import pathlib
import sys
import types

_LIB = pathlib.Path(__file__).resolve().parents[3]
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by history tests)"
    sys.modules["companion"] = _pkg

import pytest  # noqa: E402
from companion.data import db  # noqa: E402
from companion.data.event_repository import EventRepository  # noqa: E402


@pytest.fixture
def db_conn(tmp_path: pathlib.Path):
    conn = db.open_db(tmp_path / "companion.sqlite3")
    yield conn
    conn.close()


@pytest.fixture
def repo(db_conn) -> EventRepository:
    return EventRepository(db_conn)
