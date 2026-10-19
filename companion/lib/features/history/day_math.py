"""Pure day-key arithmetic — the phone mirror of spec D5/D6 (plan §15.4 D13).

UTC convention, same as the device: no timezone or DST conversion anywhere. The
input `wall_unix` comes from the DB's SYNC §4.5 reconstruction (F21 resolved it at
upsert) — this module never touches TimeSync rows and never fabricates a date.
Byte-parity with the firmware recompute is enforced by the golden fixture (C1).
"""

from __future__ import annotations

from companion.features.history.constants import (
    DAY_BOUNDARY_HOUR,
    DAYTIME_END_HOUR,
    DAYTIME_START_HOUR,
    SEC_PER_DAY,
    SEC_PER_HOUR,
)


def day_key_for(wall_unix: int) -> int:
    """D5: floor((unix + (24 − kDayBoundaryHour)·3600) / 86400).

    The logical day [04:00, 04:00) is keyed by the calendar day it ENDS on — the
    corrected firmware formula (the wave-4 PLAN-stage `+boundary` slip gave
    [20:00, 20:00) days; spec D5 carries the correction). `wall_unix == 0` is
    "unknown" and must never reach here: callers keep it as a session summary.
    """
    return (wall_unix + (SEC_PER_DAY - DAY_BOUNDARY_HOUR * SEC_PER_HOUR)) // SEC_PER_DAY


def is_daytime(wall_unix: int) -> bool:
    """D6: the episode's START hour in [09:00, 21:00) UTC counts as daytime sleep."""
    hour = (wall_unix // SEC_PER_HOUR) % 24
    return DAYTIME_START_HOUR <= hour < DAYTIME_END_HOUR
