"""C1 — golden-fixture parity (plan §15.6): the seeded-DB recompute must match the
committed expected summaries field-for-field — the phone mirror of DoD §14 / T5.
The fixture's expected values are cross-checked by an independent datetime reducer
in make_golden_fixture.py, so this test is not circular with the code under test.
"""

from __future__ import annotations

import json
import pathlib
from datetime import UTC, datetime, timedelta

from companion.features.history import build_history_view_model
from companion.features.history.day_math import day_key_for

_FIXTURE = json.loads(
    pathlib.Path(__file__).with_name("golden_fixture.json").read_text(encoding="utf-8")
)


def _seed(repo) -> None:
    repo.upsert(_FIXTURE["device_id"], [bytes.fromhex(h) for h in _FIXTURE["events_hex"]])


def test_golden_parity(repo):
    """Every committed summary field must match the recomputed view, in order."""
    _seed(repo)
    views = build_history_view_model(repo).view()
    expected = _FIXTURE["expected_summaries"]
    assert [v.day_key for v in views] == [e["day_key"] for e in expected]
    for view, want in zip(views, expected, strict=True):
        assert view.is_session == (want["day_key"] == 0)
        assert view.day_text == (
            want["day_date"] if want["day_date"] else f"unknown day (boot #{want['boot_id']})"
        )
        assert view.total_sleep_ms == want["total_sleep_ms"]
        assert view.daytime_sleep_ms == want["daytime_sleep_ms"]
        assert view.main_sleep_ms == want["main_sleep_ms"]
        assert view.episode_count == want["episode_count"]
        assert view.nap_count == want["nap_count"]
        assert view.confirmed_count == want["confirmed_count"]
        assert view.first_seq == want["first_seq"]
        assert view.last_seq == want["last_seq"]


def test_golden_wall_resolution_from_timesync(repo):
    """The boot-2 episode carried t_start_unix = 0; the repo resolved its wall time
    from the boot's TimeSync row (SYNC §4.5) and the view flags it as derived."""
    _seed(repo)
    top = build_history_view_model(repo).view()[0]
    assert top.day_text == "2026-10-16"
    (episode,) = top.episodes
    assert episode.wall_time_derived is True
    assert episode.time_text == "2026-10-15 09:23:15 UTC"


def test_golden_day_key_agrees_with_day_math(repo):
    """The fixture's day_date (audited literal) must satisfy day_math.day_key_for —
    checked at 12:00 of the *previous* calendar day, comfortably inside the
    [04:00, 04:00) logical day that ends on day_date."""
    _seed(repo)
    for view, want in zip(build_history_view_model(repo).view(),
                          _FIXTURE["expected_summaries"], strict=True):
        if want["day_key"] == 0:
            continue
        inside = datetime.fromisoformat(want["day_date"]) - timedelta(days=1, hours=-12)
        assert day_key_for(int(inside.replace(tzinfo=UTC).timestamp())) == want["day_key"]
        assert view.day_text == want["day_date"]
