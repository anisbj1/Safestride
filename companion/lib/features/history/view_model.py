"""Read-only history view-model (spec §15; D11 console renderer, D12/D14 seam).

Pages F21's `EventRepository` — the only DB seam, no feature-local SQL, no writes —
recomputes days with the pure aggregator on every call (no phone-side cursor, so
labels are always fresh), and hands out render-ready rows. Wording comes from F21's
`domain/wording`: detections read "possible"/"suspected" with a confidence value;
unknown times render "unknown" and stay a valid output, never fabricated.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.domain import wording
from companion.features.history.aggregate import DayFacts, aggregate
from companion.features.history.constants import (
    DAY_KEY_SESSION,
    DEFAULT_HISTORY_DAYS,
    EPISODE_CLASS_NAMES,
    EPISODE_LABEL_NAMES,
    EVENT_TYPE_SLEEP_EPISODE,
    HISTORY_PAGE_SIZE,
    MAX_HISTORY_EVENTS,
)

# [DERIVED] ms per minute — duration rendering ("7h 40m") for console rows.
MS_PER_MINUTE = 60_000
# [DERIVED] minutes per hour — duration rendering (mirrors wording.MINUTES_PER_HOUR).
MINUTES_PER_HOUR = 60


def duration_text(duration_ms: int) -> str:
    """Milliseconds -> "Xh YYm" console text (0 ms renders as "0h 00m", honest)."""
    total_minutes = max(0, duration_ms) // MS_PER_MINUTE
    hours, minutes = divmod(total_minutes, MINUTES_PER_HOUR)
    return f"{hours}h {minutes:02d}m"


@dataclass(frozen=True)
class EpisodeView:
    """Render-ready sleep-episode segment row (plan §15.2)."""

    seq: int
    time_text: str
    time_known: bool
    wall_time_derived: bool
    duration_ms: int
    duration_text: str
    class_text: str
    confidence_text: str
    label_text: str
    detection_text: str  # "possible sleep episode (confidence 0.82)" (spec §14)


@dataclass(frozen=True)
class NapView:
    """Render-ready nap row (D7: a NapCompleted event)."""

    seq: int
    time_text: str
    time_known: bool
    duration_ms: int
    duration_text: str


@dataclass(frozen=True)
class DayView:
    """Render-ready day row: aggregates plus its segments and naps."""

    day_key: int
    day_text: str
    is_session: bool  # True = unknown wall time, per-boot session summary (D4)
    total_sleep_ms: int
    daytime_sleep_ms: int
    main_sleep_ms: int
    episode_count: int
    nap_count: int
    confirmed_count: int
    first_seq: int
    last_seq: int
    episodes: tuple[EpisodeView, ...]
    naps: tuple[NapView, ...]


class HistoryViewModel:
    """Read-only window over the synced DB (D12: pages the F21 repository seam)."""

    def __init__(
        self,
        repository: EventRepository,
        *,
        device_id: str | None = None,
        days: int = DEFAULT_HISTORY_DAYS,
        page_size: int = HISTORY_PAGE_SIZE,
    ) -> None:
        self._repository = repository
        self._device_id = device_id
        self._days = days
        self._page_size = page_size
        self.truncated = False  # True when MAX_HISTORY_EVENTS capped the fetch

    def view(self, days: int | None = None) -> list[DayView]:
        """Newest-first day rows; session summaries (unknown time) last (D4)."""
        records, truncated = self._fetch_all()
        self.truncated = truncated
        facts = aggregate(records)
        window = self._days if days is None else days
        return [self._to_day_view(fact) for fact in _window(facts, window)]

    def _fetch_all(self) -> tuple[list, bool]:
        """Page seq DESC until exhausted; cap at MAX_HISTORY_EVENTS (never unbounded)."""
        records: list = []
        since_seq: int | None = None
        while True:
            page = self._repository.timeline(
                self._page_size, since_seq, device_id=self._device_id
            )
            records.extend(page)
            if len(page) < self._page_size:
                return records, False
            if len(records) >= MAX_HISTORY_EVENTS:
                return records[:MAX_HISTORY_EVENTS], True
            since_seq = page[-1].seq

    @staticmethod
    def _to_day_view(facts: DayFacts) -> DayView:
        is_session = facts.day_key == DAY_KEY_SESSION
        day_text = (
            f"unknown day (boot #{facts.boot_id})" if is_session else _day_date_text(facts.day_key)
        )
        return DayView(
            day_key=facts.day_key,
            day_text=day_text,
            is_session=is_session,
            total_sleep_ms=facts.total_sleep_ms,
            daytime_sleep_ms=facts.daytime_sleep_ms,
            main_sleep_ms=facts.main_sleep_ms,
            episode_count=facts.episode_count,
            nap_count=facts.nap_count,
            confirmed_count=facts.confirmed_count,
            first_seq=facts.first_seq,
            last_seq=facts.last_seq,
            episodes=tuple(_to_episode_view(f) for f in facts.episodes),
            naps=tuple(_to_nap_view(f) for f in facts.naps),
        )


# [DERIVED] Unix epoch for day-key -> date rendering (epoch + N days; avoids
# platform `fromtimestamp` year limits for large ordinals).
_EPOCH_UTC = datetime(1970, 1, 1, tzinfo=UTC)


def _day_date_text(day_key: int) -> str:
    """Day key -> the UTC calendar day it names (D5: the day the logical day ends on)."""
    return (_EPOCH_UTC + timedelta(days=day_key)).strftime("%Y-%m-%d")


def _to_episode_view(facts) -> EpisodeView:
    time_known = facts.wall_unix != 0
    return EpisodeView(
        seq=facts.seq,
        time_text=wording.event_time_text(facts.wall_unix),
        time_known=time_known,
        wall_time_derived=facts.wall_time_derived,
        duration_ms=facts.duration_ms,
        duration_text=duration_text(facts.duration_ms),
        class_text=wording.format_enum(EPISODE_CLASS_NAMES, facts.episode_class),
        confidence_text=wording.format_confidence(facts.confidence),
        label_text=wording.format_enum(EPISODE_LABEL_NAMES, facts.label),
        # Spec §14 / companion specification: detections read "possible" + confidence.
        detection_text=wording.detection_label(EVENT_TYPE_SLEEP_EPISODE, facts.confidence)
        or "possible sleep episode",
    )


def _to_nap_view(facts) -> NapView:
    return NapView(
        seq=facts.seq,
        time_text=wording.event_time_text(facts.wall_unix),
        time_known=facts.wall_unix != 0,
        duration_ms=facts.duration_ms,
        duration_text=duration_text(facts.duration_ms),
    )


def _window(facts: list[DayFacts], days: int) -> list[DayFacts]:
    """Keep the `days` most recent calendar days; session summaries always stay."""
    calendar_keys = [f.day_key for f in facts if f.day_key != DAY_KEY_SESSION]
    keep = set(sorted(set(calendar_keys), reverse=True)[:days])
    return [f for f in facts if f.day_key == DAY_KEY_SESSION or f.day_key in keep]


def build_history_view_model(
    repository: EventRepository,
    *,
    device_id: str | None = None,
    days: int = DEFAULT_HISTORY_DAYS,
) -> HistoryViewModel:
    """Documented builder seam (plan §15.3): the integrator wires this into the
    app command table at P4'; F16 never edits app.py or README itself."""
    return HistoryViewModel(repository, device_id=device_id, days=days)


def main(argv: list[str] | None = None) -> int:
    """Console entrypoint (plan §15.3): opens the local DB read-only and prints the
    history view. Exit 0 on success, including the honest "no history yet" case."""
    parser = argparse.ArgumentParser(
        prog="history",
        description="Sleep/nap history from the local synced DB (read-only, not medical advice).",
    )
    parser.add_argument("--device", default=None, help="filter by device_id (default: all)")
    parser.add_argument(
        "--days",
        type=int,
        default=DEFAULT_HISTORY_DAYS,
        help=f"most recent calendar days to print (default {DEFAULT_HISTORY_DAYS})",
    )
    args = parser.parse_args(argv)
    conn = db.open_db(db.default_db_path())
    try:
        view_model = build_history_view_model(
            EventRepository(conn), device_id=args.device, days=args.days
        )
        _print_days(view_model.view(), truncated=view_model.truncated)
    finally:
        conn.close()
    return 0


def _print_days(days: list[DayView], *, truncated: bool) -> None:
    if not days:
        print("no history yet — sync the device first (nothing is ever fabricated)")
        return
    for day in days:
        print(
            f"{day.day_text}  sleep {duration_text(day.total_sleep_ms)}"
            f" (daytime {duration_text(day.daytime_sleep_ms)},"
            f" main {duration_text(day.main_sleep_ms)})"
            f"  episodes {day.episode_count} (confirmed {day.confirmed_count})"
            f"  naps {day.nap_count}"
        )
        for episode in day.episodes:
            derived = ", wall time from TimeSync" if episode.wall_time_derived else ""
            print(
                f"  {episode.time_text}  {episode.detection_text}"
                f" - {episode.class_text}, {episode.duration_text}, {episode.label_text}{derived}"
            )
        for nap in day.naps:
            print(f"  {nap.time_text}  nap {nap.duration_text}")
    if truncated:
        print(f"note: history truncated at the {MAX_HISTORY_EVENTS}-event safety cap")
