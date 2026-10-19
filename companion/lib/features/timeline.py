"""Read-only timeline view-model (spec §7): decoded rows from the local DB, newest
first. Wall time follows SYNC §4.5 (0 = unknown, flagged); detections render as
"possible"/"suspected" **with confidence** (spec §14); unknown reason bits and enum
values surface as `reason #N` / `unknown (#n)` (spec §5).
"""

from __future__ import annotations

from dataclasses import dataclass

from companion.data.event_repository import EventRepository
from companion.data.models import EventRecord
from companion.domain import wording

# [USER] Timeline page size default (spec §8): 50 rows is one comfortable console
# page; overridable by the --limit flag at the call site.
DEFAULT_TIMELINE_LIMIT = 50


@dataclass(frozen=True)
class TimelineRow:
    """Render-ready timeline line (ui/render.py draws it)."""

    seq: int
    time_text: str
    time_known: bool
    label: str  # detection wording + confidence, plain type name, or unsupported-schema flag
    reasons_text: str
    schema_supported: bool


class TimelineViewModel:
    def __init__(
        self,
        repository: EventRepository,
        *,
        device_id: str | None = None,
        limit: int = DEFAULT_TIMELINE_LIMIT,
    ) -> None:
        self._repository = repository
        self._device_id = device_id
        self._limit = limit

    def rows(self, limit: int | None = None) -> list[TimelineRow]:
        """Newest-first page (seq DESC). Rows carry render-ready text, no logic."""
        records = self._repository.timeline(limit or self._limit, device_id=self._device_id)
        return [self._to_row(record) for record in records]

    @staticmethod
    def _to_row(record: EventRecord) -> TimelineRow:
        if not record.supported:
            # Unknown schema: raw is kept (D5) but nothing semantic is rendered —
            # guessing the layout of an unknown version is forbidden (spec §5).
            return TimelineRow(
                seq=record.seq,
                time_text="unknown",
                time_known=False,
                label=f"unsupported schema #{record.schema_version} (raw kept)",
                reasons_text="unknown",
                schema_supported=False,
            )
        detected = wording.detection_label(record.type, record.confidence)
        label = (
            detected
            if detected is not None
            else wording.format_enum(wording.EVENT_TYPE_NAMES, record.type)
        )
        return TimelineRow(
            seq=record.seq,
            time_text=wording.event_time_text(record.wall_unix),
            time_known=record.wall_unix != 0,
            label=label,
            reasons_text=wording.format_reasons(record.reasons),
            schema_supported=True,
        )
