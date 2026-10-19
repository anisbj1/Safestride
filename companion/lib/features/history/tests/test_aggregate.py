"""C3-C6 — attribution rules over decoded rows (spec D4/D7, plan §15.6):
unknown wall time stays a per-boot session summary, the D7 decode mirror filters
correctly, naps count without sleep duration, and label changes are always fresh.
"""

from __future__ import annotations

from dataclasses import replace

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.features.history.aggregate import aggregate, decode_sleep_payload
from companion.features.history.constants import DAY_KEY_SESSION
from companion.features.history.view_model import build_history_view_model

from .event_factory import pack_event, record


def _episode(seq: int, **kwargs):
    payload = kwargs.pop("payload", bytes([0, 3, 0, 0, 0, 0, 0, 0]))
    detector_id = kwargs.pop("detector_id", 8)
    return record(
        seq=seq, payload=payload, type=0, detector_id=detector_id, confidence=0.7, **kwargs
    )


# --- C3: unknown wall time -----------------------------------------------------

def test_unknown_wall_time_groups_per_boot():
    """wall_unix == 0 => session summary (day_key 0) per boot; never a date."""
    facts = aggregate([
        _episode(1, boot_id=3, duration_ms=21_600_000, wall_unix=0),
        record(seq=2, boot_id=3, duration_ms=1_200_000, t_start_unix=0, type=9, wall_unix=0),
        _episode(3, boot_id=4, duration_ms=3_600_000, wall_unix=0),
    ])
    assert [(f.day_key, f.boot_id) for f in facts] == [
        (DAY_KEY_SESSION, 3),
        (DAY_KEY_SESSION, 4),
    ]
    first = facts[0]
    assert first.total_sleep_ms == 21_600_000
    assert first.daytime_sleep_ms == 0  # no wall time => daytime cannot be attributed
    assert (first.nap_count, first.first_seq, first.last_seq) == (1, 1, 2)


def test_session_summary_renders_as_unknown_day(repo):
    """View-level half of C3: the row says 'unknown day', never a fabricated date."""
    repo.upsert("s1", [pack_event(seq=1, boot_id=7, t_start_unix=0, duration_ms=3_600_000,
                                  payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]), type=0, detector_id=8)])
    (view,) = build_history_view_model(repo).view()
    assert view.is_session is True
    assert view.day_text == "unknown day (boot #7)"
    assert view.episodes[0].time_text == "unknown"
    assert view.episodes[0].time_known is False


def test_empty_day_is_never_fabricated(repo):
    """Events that feed no summary (NapPlanned, Drowsiness) => no row at all."""
    repo.upsert("s1", [
        pack_event(seq=1, t_start_unix=1_700_000_000, type=8),   # NapPlanned
        pack_event(seq=2, t_start_unix=1_700_000_100, type=1),   # DrowsinessEvent
    ])
    assert build_history_view_model(repo).view() == []


# --- C4: D7 decode mirror ------------------------------------------------------

def test_foreign_detector_episode_skipped_entirely():
    """detector_id != 8: not counted, no totals, no seq anchor (D7)."""
    facts = aggregate([
        _episode(1, duration_ms=1_000_000, t_start_unix=1_700_000_000),
        _episode(5, duration_ms=9_999_999, t_start_unix=1_700_000_100, detector_id=9),
    ])
    (day,) = facts
    assert (day.episode_count, day.total_sleep_ms, day.first_seq, day.last_seq) == (
        1, 1_000_000, 1, 1
    )


def test_open_marker_skipped():
    facts = aggregate([
        _episode(1, duration_ms=0, t_start_unix=1_700_000_000,
                 payload=bytes([0x01, 3, 0, 0, 0, 0, 0, 0]))
    ])
    assert facts == []


def test_recovered_counts_uncertain_zero_ms():
    """Recovered flag: counts as Uncertain with 0 ms even if the payload says MainSleep."""
    facts = aggregate([
        _episode(1, duration_ms=5_000_000, t_start_unix=1_700_000_000,
                 payload=bytes([0x02, 3, 0, 0, 0, 0, 0, 0]))
    ])
    (day,) = facts
    assert (day.episode_count, day.total_sleep_ms, day.main_sleep_ms) == (1, 0, 0)
    (episode,) = day.episodes
    assert episode.episode_class == 0  # Uncertain
    assert episode.duration_ms == 0


def test_short_payload_skipped():
    """Insufficient payload: skipped, never decoded by guessing (00 §1)."""
    broken = replace(record(seq=1, t_start_unix=1_700_000_000, type=0, detector_id=8),
                     payload=bytes([0, 3]))
    assert aggregate([broken]) == []


def test_decode_sleep_payload_mirror_indices():
    payload = decode_sleep_payload(bytes([0x02, 3, 40, 0, 0, 0, 0, 0]))
    assert (payload.flags, payload.episode_class, payload.degraded_share) == (0x02, 3, 40)


# --- C5: nap counting ----------------------------------------------------------

def test_nap_completed_counted_but_adds_no_sleep_duration():
    facts = aggregate([
        record(seq=1, duration_ms=1_800_000, t_start_unix=1_700_000_000, type=9)
    ])
    (day,) = facts
    assert (day.nap_count, day.total_sleep_ms, day.episode_count) == (1, 0, 0)
    assert day.naps[0].duration_ms == 1_800_000


def test_nap_planned_never_counts():
    facts = aggregate([record(seq=1, t_start_unix=1_700_000_000, type=8)])
    assert facts == []


# --- C6: label freshness (D14: full recompute, no phone cursor) ----------------

def test_confirmed_label_change_is_always_fresh(tmp_path):
    """A re-seeded label change shows on the very next invocation — no staleness."""
    raw_v1 = [pack_event(seq=1, duration_ms=1_000_000, t_start_unix=1_700_000_000,
                         payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]), type=0, detector_id=8, label=0)]
    raw_v2 = [pack_event(seq=1, duration_ms=1_000_000, t_start_unix=1_700_000_000,
                         payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]), type=0, detector_id=8, label=1)]
    conn_a = db.open_db(tmp_path / "a.sqlite3")
    repo_a = EventRepository(conn_a)
    repo_a.upsert("d", raw_v1)
    assert build_history_view_model(repo_a).view()[0].confirmed_count == 0
    conn_a.close()
    conn_b = db.open_db(tmp_path / "b.sqlite3")
    repo_b = EventRepository(conn_b)
    repo_b.upsert("d", raw_v2)
    assert build_history_view_model(repo_b).view()[0].confirmed_count == 1
    conn_b.close()
