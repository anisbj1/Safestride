"""Colocated suite for the F22 explain half (spec §9, W5).

Registers the same source-layout package mapping as F21's `companion/tests/conftest.py`
(`companion` -> ../../..../lib) so `python -m pytest lib/features/explain/tests -q`
runs from source with no install step. F21's suite and conftest are untouched.
"""

from __future__ import annotations

import pathlib
import sys
import types

import pytest

_LIB = pathlib.Path(__file__).resolve().parents[3]
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by tests)"
    sys.modules["companion"] = _pkg

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
