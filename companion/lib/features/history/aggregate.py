"""Pure history aggregation: decoded event rows -> day facts (spec D6/D7 mirror).

No I/O, no repository, no wording: this module applies the *device's* attribution
rules (F16 spec §6 D5-D7) to synced rows — the view-model formats (wording.py), the
repository fetches. Read-only by construction; every rule here has a firmware twin
and a colocated test (C1-C6).

Unknown / insufficient data stays a valid output: a wall-less event becomes a
per-boot session summary (day_key 0), a short payload is skipped, and an event
whose schema is unsupported is never decoded (spec §10, 00 §1).
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field

from companion.data.models import EventRecord
from companion.features.history.constants import (
    DAY_KEY_SESSION,
    EPISODE_CLASS_MAIN_SLEEP,
    EPISODE_CLASS_UNCERTAIN,
    EVENT_TYPE_NAP_COMPLETED,
    EVENT_TYPE_SLEEP_EPISODE,
    SLEEP_DETECTOR_ID_MIRROR,
    SLEEP_FLAG_OPEN,
    SLEEP_FLAG_RECOVERED,
    SLEEP_PAYLOAD_CLASS_INDEX,
    SLEEP_PAYLOAD_FLAGS_INDEX,
    SLEEP_PAYLOAD_SHARE_INDEX,
    USER_LABEL_CONFIRMED,
)
from companion.features.history.day_math import day_key_for, is_daytime


@dataclass(frozen=True)
class SleepPayload:
    """D7 mirror decode of a SleepEpisode's 8-byte payload (F08 layout)."""

    flags: int
    episode_class: int
    degraded_share: int


@dataclass(frozen=True)
class EpisodeFacts:
    """One counted (closed) sleep episode — raw fields; the view-model formats."""

    seq: int
    wall_unix: int  # 0 = unknown (D4 fallback); never fabricated
    duration_ms: int  # contribution: 0 for recovered episodes (D7)
    episode_class: int  # recovered episodes forced to Uncertain (D7)
    confidence: float  # heuristic confidence 0..1, NOT clinical certainty
    label: int
    wall_time_derived: bool  # resolved from TimeSync by the repo, not carried in the event


@dataclass(frozen=True)
class NapFacts:
    """One NapCompleted event (D7: naps count, they do not add sleep duration)."""

    seq: int
    wall_unix: int
    duration_ms: int


@dataclass
class DayFacts:
    """One accumulated day (calendar day) or session summary (per boot)."""

    day_key: int
    boot_id: int  # session summaries: the boot; calendar days: 0 (spec DailySummary)
    total_sleep_ms: int = 0
    daytime_sleep_ms: int = 0
    main_sleep_ms: int = 0
    episode_count: int = 0
    nap_count: int = 0
    confirmed_count: int = 0
    first_seq: int = 0  # earliest contributing event — recompute anchor (DoD §14)
    last_seq: int = 0
    episodes: tuple[EpisodeFacts, ...] = ()
    naps: tuple[NapFacts, ...] = ()


def decode_sleep_payload(payload: bytes) -> SleepPayload:
    """D7 mirror of F08's payload layout (indices/flag bits per spec §8 constants)."""
    return SleepPayload(
        flags=payload[SLEEP_PAYLOAD_FLAGS_INDEX],
        episode_class=payload[SLEEP_PAYLOAD_CLASS_INDEX],
        degraded_share=payload[SLEEP_PAYLOAD_SHARE_INDEX],
    )


@dataclass
class _DayAcc:
    total_sleep_ms: int = 0
    daytime_sleep_ms: int = 0
    main_sleep_ms: int = 0
    episode_count: int = 0
    nap_count: int = 0
    confirmed_count: int = 0
    first_seq: int | None = None
    last_seq: int | None = None
    episodes: list[EpisodeFacts] = field(default_factory=list)
    naps: list[NapFacts] = field(default_factory=list)

    def _touch_seq(self, seq: int) -> None:
        self.first_seq = seq if self.first_seq is None else min(self.first_seq, seq)
        self.last_seq = seq if self.last_seq is None else max(self.last_seq, seq)

    def to_facts(self, day_key: int, boot_id: int) -> DayFacts:
        return DayFacts(
            day_key=day_key,
            boot_id=boot_id,
            total_sleep_ms=self.total_sleep_ms,
            daytime_sleep_ms=self.daytime_sleep_ms,
            main_sleep_ms=self.main_sleep_ms,
            episode_count=self.episode_count,
            nap_count=self.nap_count,
            confirmed_count=self.confirmed_count,
            first_seq=self.first_seq if self.first_seq is not None else 0,
            last_seq=self.last_seq if self.last_seq is not None else 0,
            episodes=tuple(self.episodes),
            naps=tuple(self.naps),
        )


def _identity(record: EventRecord) -> tuple[int, int]:
    """D4/D5 keying: known wall time -> (day_key, 0); unknown -> (0, boot_id)."""
    if record.wall_unix == 0:
        return DAY_KEY_SESSION, record.boot_id
    return day_key_for(record.wall_unix), 0


def aggregate(records: Iterable[EventRecord]) -> list[DayFacts]:
    """Recompute all day facts from rows (D14: full recompute, no phone cursor).

    Returns calendar days newest-first, then session summaries (day_key 0) in
    boot_id order — days with no counted events simply do not exist.
    """
    days: dict[tuple[int, int], _DayAcc] = {}
    for record in records:
        if not record.supported:  # unknown schema: stored raw, never decoded (spec §10)
            continue
        if record.type == EVENT_TYPE_SLEEP_EPISODE:
            if record.detector_id != SLEEP_DETECTOR_ID_MIRROR:
                continue  # foreign producer: skipped entirely, not even seq-anchored (D7)
            if len(record.payload) <= SLEEP_PAYLOAD_SHARE_INDEX:
                continue  # insufficient payload: never guess the layout (00 §1)
            payload = decode_sleep_payload(record.payload)
            if payload.flags & SLEEP_FLAG_OPEN:
                continue  # incomplete episode; its close event will arrive (D7)
            recovered = bool(payload.flags & SLEEP_FLAG_RECOVERED)
            class_index = EPISODE_CLASS_UNCERTAIN if recovered else payload.episode_class
            contribution_ms = 0 if recovered else record.duration_ms
            day_key, boot_id = _identity(record)
            acc = days.setdefault((day_key, boot_id), _DayAcc())
            acc.total_sleep_ms += contribution_ms
            if contribution_ms and record.wall_unix and is_daytime(record.wall_unix):
                acc.daytime_sleep_ms += contribution_ms  # D6: start-time attribution
            if class_index == EPISODE_CLASS_MAIN_SLEEP:
                acc.main_sleep_ms += contribution_ms
            acc.episode_count += 1
            if record.label == USER_LABEL_CONFIRMED:
                acc.confirmed_count += 1
            acc.episodes.append(
                EpisodeFacts(
                    seq=record.seq,
                    wall_unix=record.wall_unix,
                    duration_ms=contribution_ms,
                    episode_class=class_index,
                    confidence=record.confidence,
                    label=record.label,
                    wall_time_derived=record.wall_unix != 0 and record.t_start_unix == 0,
                )
            )
            acc._touch_seq(record.seq)
        elif record.type == EVENT_TYPE_NAP_COMPLETED:
            # D7: NapCompleted counts as a nap; NapPlanned is deliberately uncounted.
            day_key, boot_id = _identity(record)
            acc = days.setdefault((day_key, boot_id), _DayAcc())
            acc.nap_count += 1
            acc.naps.append(
                NapFacts(seq=record.seq, wall_unix=record.wall_unix, duration_ms=record.duration_ms)
            )
            acc._touch_seq(record.seq)
    facts = [acc.to_facts(key, boot) for (key, boot), acc in days.items()]
    facts.sort(key=lambda f: (f.day_key == DAY_KEY_SESSION, -f.day_key, f.boot_id))
    return facts
