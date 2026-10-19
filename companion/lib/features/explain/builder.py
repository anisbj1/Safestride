"""F22 companion explain view: WHY an event was raised (spec §1, D6-D9).

Read-only over F21's `EventRepository` (schema v2, decoded 48-B events). Renders,
per event: the detection label ("possible"/"suspected" **with** its confidence
value, docs/00_PROJECT_CONTEXT.md wording rule), the reason bits as name + full explanation
sentence, and unknown bits as `reason #N` (surfaced, never hidden). "Unknown /
insufficient data" is a valid rendering everywhere: unsupported schema, empty
mask (`none`), the `InsufficientEvidence` reason, and out-of-catalog bits.

Exact-seq lookup (audit F22-AUD-1) matches on (device, seq): a device scope
resolves the row and names it; without a scope the SAME seq on two devices is
ambiguous, and the ambiguity is surfaced as `AmbiguousSeq` with every candidate
row — never silently picked ("unknown / insufficient data" is a valid output).

Stable entrypoints for the composition root (D7; wired into `app.py` by the
integrator, never edited here):
    build_explanation(record, *, device_id=None)   pure, no I/O
    ExplanationViewModel(repository)     read-only view-model over the local DB
    main(argv) -> int                    standalone CLI (--seq/--limit/--device/--db)
"""

from __future__ import annotations

import argparse
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.data.models import EventRecord
from companion.domain import wording
from companion.features.explain.catalog import EXPLAIN_MAX_REASON_BITS, REASON_EXPLANATIONS

# [USER] CLI default for the recent-explanations list (spec §8): full explanations
# are multi-line, so a short page by default; --limit overrides per call.
EXPLAIN_DEFAULT_LIMIT = 10

# [DERIVED] Rows one exact-seq lookup inspects (audit F22-AUD-1): >= 2 is what
# detects that the same seq was recorded by another device — the events PK is
# (device_id, seq), so a duplicate seq can only come from a second device — and
# 16 bounds the candidate list an ambiguity surfaces (<= 16 devices is far beyond
# this prototype's 1-2 device reality; a lookup never picks among them).
EXPLAIN_SEQ_LOOKUP_LIMIT = 16


@dataclass(frozen=True)
class ReasonItem:
    """One rendered reason (spec §7): known bits carry name + explanation; unknown
    bits keep both None and render as `reason #N` (wording rule, never hidden)."""

    bit_index: int
    name: str | None
    explanation: str | None
    text: str


@dataclass(frozen=True)
class EventExplanation:
    """Render-ready explanation (F21 `TimelineRow` pattern). `reasons` is in
    ascending bit index and shows ALL set bits: the device's 3-reason cap is an
    OLED limit (D4), the companion shows the full list (D1)."""

    seq: int
    time_text: str
    time_known: bool
    label: str  # detection wording + confidence, plain type name, or unsupported-schema flag
    confidence_text: str  # "" only when nothing semantic is rendered (unsupported schema)
    schema_supported: bool
    reasons: tuple[ReasonItem, ...]
    device_id: str | None = None  # scope this row resolved against; None = no scope was
    # given and F21's read seam (`EventRecord`) carries no device_id — stated, never guessed


@dataclass(frozen=True)
class AmbiguousSeq:
    """Audit F22-AUD-1: the same per-device seq exists on two or more devices and
    no device scope was given, so WHICH event is meant is unknown. `candidates`
    holds one render-ready row per device (up to `EXPLAIN_SEQ_LOOKUP_LIMIT`);
    nothing is picked, nothing is guessed. Candidate device ids are not
    observable through F21's frozen seam (the repository drops `device_id` when
    decoding rows), so the caller resolves the ambiguity by re-querying with a
    device scope — that scope is what names a device in `EventExplanation`."""

    seq: int
    candidates: tuple[EventExplanation, ...]


def build_explanation(record: EventRecord, *, device_id: str | None = None) -> EventExplanation:
    """Pure record -> explanation (no I/O). Unknown/insufficient data stays a
    valid output; confidence is always a value, never a certainty claim.
    `device_id` is the scope the row was resolved under (None = unscoped)."""
    if not record.supported:
        # Unknown schema: raw is kept by F21, nothing semantic is rendered -
        # guessing the layout of an unknown version is forbidden (spec §5).
        return EventExplanation(
            seq=record.seq,
            time_text="unknown",
            time_known=False,
            label=f"unsupported schema #{record.schema_version} (raw kept)",
            confidence_text="",
            schema_supported=False,
            reasons=(),
            device_id=device_id,
        )
    detected = wording.detection_label(record.type, record.confidence)
    if detected is not None:
        label = detected
    else:
        label = wording.format_enum(wording.EVENT_TYPE_NAMES, record.type)
    return EventExplanation(
        seq=record.seq,
        time_text=wording.event_time_text(record.wall_unix),
        time_known=record.wall_unix != 0,
        label=label,
        confidence_text=wording.format_confidence(record.confidence),
        schema_supported=True,
        reasons=_reason_items(record.reasons),
        device_id=device_id,
    )


def _reason_items(mask: int) -> tuple[ReasonItem, ...]:
    """ReasonMask -> ascending-bit items; unknown bits render `reason #N` inline
    at their own position (D4: position carries meaning)."""
    items: list[ReasonItem] = []
    for bit in range(EXPLAIN_MAX_REASON_BITS):
        if not mask & (1 << bit):
            continue
        sentence = REASON_EXPLANATIONS.get(bit)
        if sentence is not None and bit < len(wording.REASON_NAMES):
            name = wording.REASON_NAMES[bit]
            items.append(ReasonItem(bit, name, sentence, f"{name}: {sentence}"))
        else:
            items.append(ReasonItem(bit, None, None, f"{wording.UNKNOWN_REASON_PREFIX}{bit}"))
    return tuple(items)


def render_explanation(explanation: EventExplanation) -> str:
    """Multi-line console rendering of one explanation (CLI + tests; the future
    UI consumes the dataclasses directly). The device scope is part of the head
    when one was given — the row is then named, not inferred (F22-AUD-1)."""
    head = f"seq {explanation.seq}  {explanation.time_text}"
    if explanation.device_id is not None:
        head += f"  device {explanation.device_id}"
    lines = [head, f"  {explanation.label}"]
    if not explanation.schema_supported:
        return "\n".join(lines)
    lines.append(f"  confidence {explanation.confidence_text}")
    if explanation.reasons:
        lines.append("  reasons:")
        lines.extend(f"    - {item.text}" for item in explanation.reasons)
    else:
        # Empty mask is a valid rendering: the device recorded no reasons and
        # nothing is ever back-filled with guesses (spec §7).
        lines.append("  reasons: none")
    return "\n".join(lines)


def render_ambiguous(lookup: AmbiguousSeq) -> str:
    """Console rendering of an ambiguous exact-seq lookup: EVERY candidate row is
    shown, none is picked (F22-AUD-1). Candidate device ids are not observable
    through F21's read seam, so the text says what is unknown instead of naming
    a device it cannot see."""
    lines = [
        f"seq {lookup.seq}  ambiguous",
        f"  recorded by {len(lookup.candidates)} devices; which one is meant is "
        "unknown - no row picked",
        "  candidates:",
    ]
    for index, candidate in enumerate(lookup.candidates, start=1):
        lines.append(f"    [{index}]")
        lines.extend(f"      {line}" for line in render_explanation(candidate).splitlines())
    lines.append("  pass --device <id> to resolve")
    return "\n".join(lines)


class ExplanationViewModel:
    """Read-only view-model over F21's repository (D7): renders explanations for
    synced events; never writes, never reaches into the schema."""

    def __init__(
        self,
        repository: EventRepository,
        *,
        device_id: str | None = None,
        limit: int = EXPLAIN_DEFAULT_LIMIT,
    ) -> None:
        self._repository = repository
        self._device_id = device_id
        self._limit = limit

    def for_seq(
        self, seq: int, *, device_id: str | None = None
    ) -> EventExplanation | AmbiguousSeq | None:
        """Exact (device, seq) lookup (F22-AUD-1), never guessed:
        - a device scope (per call, else the view's) resolves exactly one row and
          names it via `EventExplanation.device_id`;
        - no scope + the same seq recorded by two devices (PK `device_id, seq`
          makes duplicates a second device) -> `AmbiguousSeq` with every
          candidate: both are surfaced, neither is picked;
        - no such seq -> None.
        `device_id` here overrides the view's scope for this call."""
        scope = device_id if device_id is not None else self._device_id
        records = self._repository.timeline(
            EXPLAIN_SEQ_LOOKUP_LIMIT, since_seq=seq + 1, device_id=scope
        )
        if not records or records[0].seq != seq:
            return None
        if len(records) > 1 and records[1].seq == seq:
            # Two rows share this seq => two devices recorded it (PK) => which
            # event is meant is UNKNOWN without a scope: surface them all.
            return AmbiguousSeq(
                seq=seq,
                candidates=tuple(
                    build_explanation(record) for record in records if record.seq == seq
                ),
            )
        return build_explanation(records[0], device_id=scope)

    def rows(self, limit: int | None = None) -> list[EventExplanation]:
        """Newest-first page (seq DESC), same semantics as the timeline view."""
        records = self._repository.timeline(limit or self._limit, device_id=self._device_id)
        return [build_explanation(record, device_id=self._device_id) for record in records]


def main(argv: list[str] | None = None, *, db_path: Path | str | None = None) -> int:
    """Standalone CLI (D7). Default DB path mirrors `app.py`: --db, then the
    injected path, then `ANAROLEPSY_COMPANION_DB` / the OS user data dir."""
    parser = argparse.ArgumentParser(
        prog="anarolepsy-explain",
        description=(
            "WHY an event was raised: reason explanations from the local synced DB "
            "(not a medical device; observes only)."
        ),
    )
    parser.add_argument("--seq", type=int, default=None, help="explain exactly this event seq")
    parser.add_argument("--limit", type=int, default=EXPLAIN_DEFAULT_LIMIT)
    parser.add_argument("--device", default=None, help="filter by device_id")
    parser.add_argument("--db", default=None, help="DB path (default: app DB location)")
    args = parser.parse_args(argv)
    if args.db is not None:
        path = args.db
    elif db_path is not None:
        path = db_path
    else:
        path = db.default_db_path()
    conn = db.open_db(path)
    try:
        view_model = ExplanationViewModel(
            EventRepository(conn), device_id=args.device, limit=args.limit
        )
        if args.seq is not None:
            result = view_model.for_seq(args.seq)
            if result is None:
                print(f"event seq {args.seq} not found")
                return 1
            if isinstance(result, AmbiguousSeq):
                # Spec §10: ambiguous without a device scope -> exit 1, every
                # candidate rendered, no row picked (F22-AUD-1).
                print(render_ambiguous(result))
                return 1
            print(render_explanation(result))
            return 0
        rows = view_model.rows()
        if not rows:
            print("no events synced yet")
            return 0
        for explanation in rows:
            print(render_explanation(explanation))
    except sqlite3.Error as exc:  # locked/corrupt: report, stay responsive (F21 §10)
        print(f"database error: {exc}")
        return 1
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
