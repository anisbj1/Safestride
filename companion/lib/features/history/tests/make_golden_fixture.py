"""Regenerate `golden_fixture.json` — run MANUALLY, the committed JSON is the golden.

The expected summaries are cross-checked by an INDEPENDENT reducer written against
the spec's semantic rules with `datetime` arithmetic (day = [04:00, 04:00) keyed by
the day it ends on) — not against `day_math.day_key_for` — so the fixture cannot
become circular with the code under test. Value-level parity with the frozen
firmware `bw_history` C++ fixture is an H9 review item (spec §15.4 D15 `[ASSUMED]`).
"""

from __future__ import annotations

import json
import pathlib
import sys
import types
from datetime import UTC, date, datetime, timedelta

# Source-layout bootstrap (same as conftest.py) so the generator also runs standalone.
_LIB = pathlib.Path(__file__).resolve().parents[3]
if "companion" not in sys.modules:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]
    sys.modules["companion"] = _pkg

from event_factory import pack_event  # noqa: E402

OUT = pathlib.Path(__file__).with_name("golden_fixture.json")

DEVICE_ID = "f16golden"

EVENT_TYPE_SLEEP_EPISODE = 0
EVENT_TYPE_DROWSINESS = 1
EVENT_TYPE_NAP_COMPLETED = 9
EVENT_TYPE_TIMESYNC = 10
SLEEP_DETECTOR_ID = 8


def unix(year: int, month: int, day: int, hour: int = 0, minute: int = 0) -> int:
    return int(datetime(year, month, day, hour, minute, tzinfo=UTC).timestamp())


def day_key_semantic(wall_unix: int) -> int:
    """Spec D5 by its SEMANTIC rule: [04:00, 04:00) keyed by the day it ends on.

    Expressed in epoch-days (days since 1970-01-01) — the same numbering the
    firmware's `floor((unix + 72000)/86400)` produces.
    """
    shifted = (datetime.fromtimestamp(wall_unix, tz=UTC) + timedelta(hours=20)).date()
    return (shifted - date(1970, 1, 1)).days


# --- the event stream (boot 1: explicit wall times; boot 2: TimeSync-resolved;
# --- boot 3: no TimeSync -> session summaries, never a fabricated date) ----------
EVENTS: list[dict] = [
    # main sleep 23:30 -> 06:50, lands in the logical day that ENDS on 10-14 (D5)
    dict(seq=1, boot_id=1, t_start_ms=1_000, duration_ms=26_400_000,
         t_start_unix=unix(2026, 10, 13, 23, 30), confidence=0.82,
         payload=bytes([0, 3, 10, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         detector_id=SLEEP_DETECTOR_ID),
    # nap 13:05 (daytime by D6, but naps only bump nap_count — D7)
    dict(seq=2, boot_id=1, t_start_ms=2_000, duration_ms=1_800_000,
         t_start_unix=unix(2026, 10, 14, 13, 5), type=EVENT_TYPE_NAP_COMPLETED),
    # rest episode 15:00 confirmed -> daytime of the day ending 10-15
    dict(seq=3, boot_id=1, t_start_ms=3_000, duration_ms=2_700_000,
         t_start_unix=unix(2026, 10, 14, 15, 0), confidence=0.55,
         payload=bytes([0, 1, 40, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         label=1, detector_id=SLEEP_DETECTOR_ID),
    # open marker: incomplete, skipped entirely (D7)
    dict(seq=4, boot_id=1, t_start_ms=4_000, duration_ms=0,
         t_start_unix=unix(2026, 10, 14, 16, 0),
         payload=bytes([0x01, 3, 0, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         detector_id=SLEEP_DETECTOR_ID),
    # foreign producer (detector_id 9): skipped entirely, not counted, no seq anchor (D7)
    dict(seq=5, boot_id=1, t_start_ms=5_000, duration_ms=9_999_999,
         t_start_unix=unix(2026, 10, 14, 17, 0),
         payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         detector_id=9),
    # recovered: counts as Uncertain with 0 ms; its 5 000 000 ms must NOT be summed (D7)
    dict(seq=6, boot_id=1, t_start_ms=6_000, duration_ms=5_000_000,
         t_start_unix=unix(2026, 10, 15, 1, 30), confidence=0.40,
         payload=bytes([0x02, 3, 0, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         detector_id=SLEEP_DETECTOR_ID),
    # boot 2: TimeSync anchor; the episode below has t_start_unix = 0
    dict(seq=7, boot_id=2, t_start_ms=5_000, t_start_unix=unix(2026, 10, 15, 8, 0),
         type=EVENT_TYPE_TIMESYNC),
    # wall resolved = 08:00 + 4995 s = 09:23:15 on 10-15 -> daytime, day ending 10-16
    dict(seq=8, boot_id=2, t_start_ms=5_000_000, t_start_unix=0, duration_ms=7_200_000,
         confidence=0.91, payload=bytes([0, 3, 5, 0, 0, 0, 0, 0]),
         type=EVENT_TYPE_SLEEP_EPISODE, label=1, detector_id=SLEEP_DETECTOR_ID),
    # boot 3 (no TimeSync): a non-history event and two unknown-wall rows
    dict(seq=9, boot_id=3, t_start_ms=1_000, t_start_unix=0, type=EVENT_TYPE_DROWSINESS),
    dict(seq=10, boot_id=3, t_start_ms=20_000_000, t_start_unix=0, duration_ms=21_600_000,
         confidence=0.70, payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]),
         type=EVENT_TYPE_SLEEP_EPISODE, detector_id=SLEEP_DETECTOR_ID),
    dict(seq=11, boot_id=3, t_start_ms=21_000_000, t_start_unix=0, duration_ms=1_200_000,
         type=EVENT_TYPE_NAP_COMPLETED),
    # unsupported schema: stored raw, never decoded as data (spec §10)
    dict(seq=12, boot_id=3, t_start_unix=unix(2026, 10, 16, 12, 0),
         payload=bytes([0, 3, 0, 0, 0, 0, 0, 0]), type=EVENT_TYPE_SLEEP_EPISODE,
         detector_id=SLEEP_DETECTOR_ID, schema_version=2),
]

# --- the committed golden: audited literals, cross-checked by the reducer below ---
EXPECTED_SUMMARIES: list[dict] = [
    dict(day_date="2026-10-16", day_key=day_key_semantic(unix(2026, 10, 16, 0, 0)), boot_id=0,
         total_sleep_ms=7_200_000, daytime_sleep_ms=7_200_000, main_sleep_ms=7_200_000,
         episode_count=1, nap_count=0, confirmed_count=1, first_seq=8, last_seq=8),
    dict(day_date="2026-10-15", day_key=day_key_semantic(unix(2026, 10, 15, 0, 0)), boot_id=0,
         total_sleep_ms=2_700_000, daytime_sleep_ms=2_700_000, main_sleep_ms=0,
         episode_count=2, nap_count=1, confirmed_count=1, first_seq=2, last_seq=6),
    dict(day_date="2026-10-14", day_key=day_key_semantic(unix(2026, 10, 14, 0, 0)), boot_id=0,
         total_sleep_ms=26_400_000, daytime_sleep_ms=0, main_sleep_ms=26_400_000,
         episode_count=1, nap_count=0, confirmed_count=0, first_seq=1, last_seq=1),
    dict(day_date=None, day_key=0, boot_id=3,
         total_sleep_ms=21_600_000, daytime_sleep_ms=0, main_sleep_ms=21_600_000,
         episode_count=1, nap_count=1, confirmed_count=0, first_seq=10, last_seq=11),
]


def resolve_wall(event: dict, timesyncs: dict[int, list[tuple[int, int]]]) -> int:
    """Independent SYNC §4.5 mirror: explicit unix, else nearest TimeSync of the boot."""
    if event.get("t_start_unix"):
        return int(event["t_start_unix"])
    candidates = [point for point in timesyncs.get(event["boot_id"], []) if point[1] != 0]
    if not candidates:
        return 0
    best = min(candidates, key=lambda p: (abs(event["t_start_ms"] - p[0]), p[0]))
    return best[1] + (event["t_start_ms"] - best[0]) // 1000


def reduce_independent() -> list[dict]:
    """Second implementation of D4-D7 with datetime semantics (cross-check only)."""
    timesyncs: dict[int, list[tuple[int, int]]] = {}
    for event in EVENTS:
        if event["type"] == EVENT_TYPE_TIMESYNC:
            timesyncs.setdefault(event["boot_id"], []).append(
                (event["t_start_ms"], event["t_start_unix"])
            )
    days: dict[tuple[int, int], dict] = {}

    def acc_for(event: dict) -> dict:
        wall = resolve_wall(event, timesyncs)
        key = (day_key_semantic(wall), 0) if wall else (0, event["boot_id"])
        return days.setdefault(
            key,
            dict(day_key=key[0], boot_id=key[1], total_sleep_ms=0, daytime_sleep_ms=0,
                 main_sleep_ms=0, episode_count=0, nap_count=0, confirmed_count=0,
                 first_seq=event["seq"], last_seq=event["seq"]),
        )

    for event in EVENTS:
        if event.get("schema_version", 1) != 1:
            continue
        if event["type"] == EVENT_TYPE_TIMESYNC:
            continue
        if event["type"] == EVENT_TYPE_NAP_COMPLETED:
            acc = acc_for(event)
            acc["nap_count"] += 1
            acc["first_seq"] = min(acc["first_seq"], event["seq"])
            acc["last_seq"] = max(acc["last_seq"], event["seq"])
            continue
        if event["type"] != EVENT_TYPE_SLEEP_EPISODE or event["detector_id"] != SLEEP_DETECTOR_ID:
            continue
        flags, class_index = event["payload"][0], event["payload"][1]
        if flags & 0x01:
            continue
        wall = resolve_wall(event, timesyncs)
        acc = acc_for(event)
        acc["first_seq"] = min(acc["first_seq"], event["seq"])
        acc["last_seq"] = max(acc["last_seq"], event["seq"])
        recovered = bool(flags & 0x02)
        contribution = 0 if recovered else event["duration_ms"]
        class_index = 0 if recovered else class_index
        acc["total_sleep_ms"] += contribution
        hour = ((wall // 3600) % 24) if wall else -1
        if contribution and 9 <= hour < 21:
            acc["daytime_sleep_ms"] += contribution
        if class_index == 3:
            acc["main_sleep_ms"] += contribution
        acc["episode_count"] += 1
        if event.get("label", 0) == 1:
            acc["confirmed_count"] += 1
    facts = list(days.values())
    facts.sort(key=lambda f: (f["day_key"] == 0, -f["day_key"], f["boot_id"]))
    return facts


def main() -> None:
    computed = reduce_independent()
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    for expected in EXPECTED_SUMMARIES:
        derived_date = (
            (epoch + timedelta(days=expected["day_key"])).date().isoformat()
            if expected["day_key"]
            else None
        )
        assert expected["day_date"] == derived_date, expected
    assert computed == [
        {key: value for key, value in expected.items() if key != "day_date"}
        for expected in EXPECTED_SUMMARIES
    ], f"independent reducer disagrees with golden literals:\n{computed}\nvs\n{EXPECTED_SUMMARIES}"
    fixture = {
        "note": (
            "F16 companion golden fixture (spec §7/§15.4 D15): raw 48 B events + the "
            "expected day summaries. Regenerate with make_golden_fixture.py (manual); "
            "value-level parity with the firmware bw_history fixture is an H9 item."
        ),
        "device_id": DEVICE_ID,
        "events_hex": [pack_event(**event).hex() for event in EVENTS],
        "expected_summaries": EXPECTED_SUMMARIES,
    }
    OUT.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT.name}: {len(EVENTS)} events, {len(EXPECTED_SUMMARIES)} summaries")


if __name__ == "__main__":
    main()
