"""§9 test 5 — wall-time reconstruction, SYNC_PROTOCOL.md §4.5 (D6): direct unix,
nearest TimeSync of the same boot, tie → earlier, signed ms math before the sync,
no TimeSync → unknown(0), other boot_id ignored."""

from __future__ import annotations

from companion.data.models import TimeSyncPoint
from companion.domain.time_reconstruct import resolve_wall_unix


def test_direct_t_start_unix_wins():
    # §4.5 rule 1: a non-zero t_start_unix is the wall time; TimeSyncs are irrelevant.
    wall = resolve_wall_unix(
        t_start_ms=10_000,
        t_start_unix=1_600_000_000,
        boot_id=3,
        timesyncs=[TimeSyncPoint(boot_id=3, t_mono_ms=1, unix_s=5)],
    )
    assert wall == 1_600_000_000


def test_nearest_timesync_of_same_boot():
    # Event at 10 s mono; TimeSyncs at 5 s (unix 1000) and 9 s (unix 1005) →
    # nearest is 9 s → wall = 1005 + (10000-9000)/1000 = 1006.
    wall = resolve_wall_unix(
        t_start_ms=10_000,
        t_start_unix=0,
        boot_id=3,
        timesyncs=[
            TimeSyncPoint(boot_id=3, t_mono_ms=5_000, unix_s=1000),
            TimeSyncPoint(boot_id=3, t_mono_ms=9_000, unix_s=1005),
        ],
    )
    assert wall == 1006


def test_tie_picks_the_earlier_timesync():
    # |10000-8000| == |12000-10000| → the earlier TimeSync (8 s, unix 1000) wins.
    # The two points sit on DIFFERENT clock lines (earlier: unix - mono/1000 = 992,
    # later: 1006 - 12 = 994), so only the tie-break key can produce 1002:
    # earlier line gives 992 + 10 = 1002, later line would give 994 + 10 = 1004.
    # Reverting the key to plain abs(...) returns the later point → FAILS here
    # (audit F21-AUD-01).
    wall = resolve_wall_unix(
        t_start_ms=10_000,
        t_start_unix=0,
        boot_id=3,
        timesyncs=[
            TimeSyncPoint(boot_id=3, t_mono_ms=12_000, unix_s=1006),
            TimeSyncPoint(boot_id=3, t_mono_ms=8_000, unix_s=1000),
        ],
    )
    assert wall == 1002


def test_event_before_its_timesync_uses_signed_math():
    # Event 2500 ms BEFORE the TimeSync: delta = -2500 → signed ms math truncates
    # toward zero (C semantics, not Python floor) → -2 s → wall = 1000 - 2 = 998.
    wall = resolve_wall_unix(
        t_start_ms=1_000,
        t_start_unix=0,
        boot_id=3,
        timesyncs=[TimeSyncPoint(boot_id=3, t_mono_ms=3_500, unix_s=1000)],
    )
    assert wall == 998


def test_no_timesync_is_unknown_zero():
    # §4.5 rule 3: unknown, stored as 0 and flagged in the UI — never fabricated.
    assert (
        resolve_wall_unix(t_start_ms=5_000, t_start_unix=0, boot_id=3, timesyncs=[]) == 0
    )


def test_timesyncs_of_other_boots_are_ignored():
    wall = resolve_wall_unix(
        t_start_ms=5_000,
        t_start_unix=0,
        boot_id=3,
        timesyncs=[TimeSyncPoint(boot_id=9, t_mono_ms=5_000, unix_s=1000)],
    )
    assert wall == 0


def test_timesync_with_unknown_unix_is_ignored():
    # A TimeSync that never got a valid clock contributes nothing (never fabricates).
    wall = resolve_wall_unix(
        t_start_ms=5_000,
        t_start_unix=0,
        boot_id=3,
        timesyncs=[TimeSyncPoint(boot_id=3, t_mono_ms=5_000, unix_s=0)],
    )
    assert wall == 0
