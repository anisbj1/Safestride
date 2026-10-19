"""C2 — day-boundary and daytime-window mirror (spec D5/D6, plan §15.6).

The phone must key days exactly like the device: [04:00, 04:00) UTC keyed by the
calendar day the logical day ENDS on; daytime = episode start hour in [09:00, 21:00).
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

from companion.features.history import day_math
from companion.features.history.aggregate import aggregate
from companion.features.history.constants import SEC_PER_DAY, SEC_PER_HOUR

from .event_factory import record

_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)


def _unix(year: int, month: int, day: int, hour: int = 0, minute: int = 0) -> int:
    return int(datetime(year, month, day, hour, minute, tzinfo=UTC).timestamp())


def test_exact_boundary_edges():
    """[04:00, 04:00): 03:59:59 stays in the old day, 04:00:00 opens the new one."""
    assert day_math.day_key_for(SEC_PER_HOUR * 3 + 59 * 60 + 59) == 0
    assert day_math.day_key_for(SEC_PER_HOUR * 4) == 1
    assert day_math.day_key_for(0) == 0
    assert day_math.day_key_for(SEC_PER_DAY) == 1


def test_day_key_matches_semantic_rule():
    """Formula output equals the spec's semantic rule: (start + 20 h) -> calendar date."""
    for wall in (
        _unix(2026, 10, 13, 23, 30),  # late night -> the day it ends on
        _unix(2026, 10, 14, 3, 59),   # just before the boundary
        _unix(2026, 10, 14, 4, 0),    # exactly at the boundary
        _unix(2026, 10, 15, 12, 0),   # midday
    ):
        shifted_date = (datetime.fromtimestamp(wall, tz=UTC) + timedelta(hours=20)).date()
        assert day_math.day_key_for(wall) == (shifted_date - _EPOCH.date()).days


def test_daytime_window_edges():
    """D6 window [09:00, 21:00): start inclusive, end exclusive."""
    assert not day_math.is_daytime(_unix(2026, 1, 5, 8, 59))  # 08:59
    assert day_math.is_daytime(_unix(2026, 1, 5, 9, 0))
    assert day_math.is_daytime(_unix(2026, 1, 5, 20, 59))
    assert not day_math.is_daytime(_unix(2026, 1, 5, 21, 0))


def test_multi_day_episode_wholly_on_start_day():
    """An episode crossing the boundary contributes its full duration to its start day."""
    facts = aggregate([
        record(
            seq=1,
            duration_ms=30 * 3600 * 1000,  # 30 h, 23:00 -> 05:00 two days later
            t_start_unix=_unix(2026, 10, 13, 23, 0),
            confidence=0.6,
            payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]),
            type=0,
            detector_id=8,
        )
    ])
    assert len(facts) == 1
    (day,) = facts
    assert day.total_sleep_ms == 30 * 3600 * 1000
    assert day.day_key == day_math.day_key_for(_unix(2026, 10, 13, 23, 0))


def test_daytime_attribution_by_start_time():
    """09:30 start counts as daytime; 08:59 does not — attribution by START hour."""
    daytime = aggregate([
        record(seq=1, duration_ms=3_600_000, t_start_unix=_unix(2026, 1, 5, 9, 30),
               payload=bytes([0, 1, 0, 0, 0, 0, 0, 0]), type=0, detector_id=8)
    ])
    morning = aggregate([
        record(seq=1, duration_ms=3_600_000, t_start_unix=_unix(2026, 1, 5, 8, 59),
               payload=bytes([0, 1, 0, 0, 0, 0, 0, 0]), type=0, detector_id=8)
    ])
    assert daytime[0].daytime_sleep_ms == 3_600_000
    assert morning[0].daytime_sleep_ms == 0
