"""C7-C9 — repository seam, wording, and honest-empty behavior (plan §15.6).

C7 proves the view is read-only over F21's DB; C8 proves the wording rules
("possible" + confidence, unknown never faked, no diagnostic terms); C9 proves the
empty/unsupported cases are valid outputs, never fabricated data.
"""

from __future__ import annotations

import json
import pathlib

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.domain import wording
from companion.features.history.view_model import build_history_view_model, main

from .event_factory import pack_event

_FIXTURE = json.loads(
    pathlib.Path(__file__).with_name("golden_fixture.json").read_text(encoding="utf-8")
)


def _seed(repo) -> None:
    repo.upsert(_FIXTURE["device_id"], [bytes.fromhex(h) for h in _FIXTURE["events_hex"]])


def _snapshot(conn) -> dict[str, list[tuple]]:
    return {
        table: [tuple(row) for row in conn.execute(f"SELECT * FROM {table}").fetchall()]
        for table in ("events", "sync_state", "devices")
    }


# --- C7: read-only over F21's repository (D12) ---------------------------------

def test_view_makes_no_writes(db_conn, repo):
    """Full table snapshots before/after a view build: byte-identical (no writes)."""
    _seed(repo)
    before = _snapshot(db_conn)
    build_history_view_model(repo).view()
    assert _snapshot(db_conn) == before


# --- C8: wording rules ---------------------------------------------------------

def test_detection_wording_possible_with_confidence(repo):
    _seed(repo)
    episodes = [ep for day in build_history_view_model(repo).view() for ep in day.episodes]
    assert episodes  # fixture has counted episodes
    for episode in episodes:
        assert episode.detection_text.startswith("possible sleep episode (confidence ")
        assert episode.confidence_text  # a confidence value is always present


def test_unknown_time_rendered_unknown_never_faked(repo):
    _seed(repo)
    sessions = [day for day in build_history_view_model(repo).view() if day.is_session]
    assert sessions
    for day in sessions:
        assert day.day_text.startswith("unknown day (boot #")
        for episode in day.episodes:
            assert episode.time_text == "unknown"
            assert episode.time_known is False


def _all_rendered_strings(views) -> list[str]:
    strings: list[str] = []
    for day in views:
        strings.extend([day.day_text])
        for episode in day.episodes:
            strings.extend([episode.time_text, episode.detection_text, episode.class_text,
                            episode.duration_text, episode.label_text, episode.confidence_text])
        for nap in day.naps:
            strings.extend([nap.time_text, nap.duration_text])
    return strings


def test_no_banned_diagnostic_terms_anywhere(repo, monkeypatch, tmp_path, capsys):
    """View rows AND the console output: no diagnosis-family wording (spec §5)."""
    _seed(repo)
    views = build_history_view_model(repo).view()
    banned = tuple(term.casefold() for term in wording.BANNED_DIAGNOSTIC_TERMS)
    for text in _all_rendered_strings(views):
        assert not any(term in text.casefold() for term in banned), text
    db_path = tmp_path / "cli.sqlite3"
    monkeypatch.setenv(db.ENV_DB_PATH, str(db_path))
    conn = db.open_db(db_path)
    EventRepository(conn).upsert(
        _FIXTURE["device_id"], [bytes.fromhex(h) for h in _FIXTURE["events_hex"]]
    )
    conn.close()
    assert main([]) == 0
    printed = capsys.readouterr().out
    assert "possible sleep episode (confidence 0.82)" in printed
    for line in printed.splitlines():
        assert not any(term in line.casefold() for term in banned), line


# --- C9: empty / unsupported are valid outputs ---------------------------------

def test_empty_db_yields_no_days(repo):
    assert build_history_view_model(repo).view() == []


def test_main_on_empty_db_says_no_history(monkeypatch, tmp_path, capsys):
    monkeypatch.setenv(db.ENV_DB_PATH, str(tmp_path / "empty.sqlite3"))
    assert main([]) == 0
    assert "no history yet" in capsys.readouterr().out


def test_unsupported_schema_rows_stored_but_never_rendered(repo):
    repo.upsert("d", [pack_event(seq=1, t_start_unix=1_700_000_000,
                                 payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]),
                                 type=0, detector_id=8, schema_version=2)])
    assert repo.event_count("d") == 1  # raw kept (F21 D5)
    assert build_history_view_model(repo).view() == []  # never decoded as data
