"""§9 test 9 — timeline view-model: mixed wall times (known vs flagged unknown),
detection rows render "possible/suspected" + confidence (spec §14 DoD), unknown
ReasonCode → "reason #N" (never hidden), unsupported schema flagged (spec §5)."""

from __future__ import annotations

from companion.data.event_repository import EventRepository
from companion.domain import wording
from companion.features.timeline import TimelineViewModel
from companion.ui import render
from vectors import make_event_bytes

WALL_KNOWN = 1_760_000_000


def _vm(repo: EventRepository) -> TimelineViewModel:
    return TimelineViewModel(repo, device_id="dev1")


def test_mixed_wall_times_newest_first(repo):
    repo.upsert(
        "dev1",
        [
            make_event_bytes(seq=1, type=1, t_start_unix=WALL_KNOWN),
            make_event_bytes(seq=2, type=1, t_start_unix=0),  # clock never synced
        ],
    )
    rows = _vm(repo).rows()
    assert [row.seq for row in rows] == [2, 1]  # seq DESC
    assert rows[0].time_known is False
    assert rows[0].time_text == "unknown"  # SYNC §4.5 rule 3: flagged, never fabricated
    assert rows[1].time_known is True
    assert rows[1].time_text == wording.event_time_text(WALL_KNOWN)


def test_detection_rows_render_possible_with_confidence(repo):
    repo.upsert(
        "dev1",
        [
            make_event_bytes(seq=1, type=1, confidence=0.75, t_start_unix=WALL_KNOWN),
            make_event_bytes(seq=2, type=2, confidence=0.80, t_start_unix=WALL_KNOWN),
            make_event_bytes(seq=3, type=0, confidence=0.55, t_start_unix=WALL_KNOWN),
        ],
    )
    rendered = render.render_timeline(_vm(repo).rows())
    assert "possible drowsiness (confidence 0.75)" in rendered
    assert "suspected cataplexy-like event (confidence 0.80)" in rendered
    assert "possible sleep episode (confidence 0.55)" in rendered


def test_timesync_reconstruction_renders_wall_time(repo):
    repo.upsert(
        "dev1",
        [
            # TimeSync at mono 5000 → later resolves the earlier unknown-clock event.
            make_event_bytes(
                seq=1, type=10, t_start_ms=5_000, t_start_unix=WALL_KNOWN, boot_id=3
            ),
            make_event_bytes(seq=2, type=1, t_start_ms=7_000, t_start_unix=0, boot_id=3),
        ],
    )
    rows = {row.seq: row for row in _vm(repo).rows()}
    assert rows[2].time_known is True
    assert rows[2].time_text == wording.event_time_text(WALL_KNOWN + 2)
    assert "(time unknown)" not in render.render_timeline(list(rows.values()))


def test_unknown_reason_bit_renders_as_reason_n(repo):
    repo.upsert(
        "dev1",
        [make_event_bytes(seq=1, type=1, reasons=(1 << 1) | (1 << 20), t_start_unix=WALL_KNOWN)],
    )
    row = _vm(repo).rows()[0]
    assert row.reasons_text == "ProlongedInactivity, reason #20"  # bit 1 known, bit 20 not


def test_unknown_event_type_renders_as_unknown_n(repo):
    repo.upsert("dev1", [make_event_bytes(seq=1, type=99, t_start_unix=WALL_KNOWN)])
    row = _vm(repo).rows()[0]
    assert row.label == "unknown (#99)"  # never coerced to the nearest known type


def test_non_detection_event_renders_plain_name_without_confidence(repo):
    repo.upsert("dev1", [make_event_bytes(seq=1, type=11, t_start_unix=WALL_KNOWN)])
    row = _vm(repo).rows()[0]
    assert row.label == "Boot"
    assert "confidence" not in row.label


def test_unsupported_schema_row_is_flagged_not_decoded(repo):
    repo.upsert("dev1", [make_event_bytes(seq=1, type=1, schema_version=2)])
    row = _vm(repo).rows()[0]
    assert not row.schema_supported
    assert row.label.startswith("unsupported schema #2")
    assert row.time_known is False  # semantic fields of an unknown schema are untrusted


def test_empty_timeline_is_a_valid_output(repo):
    assert render.render_timeline(_vm(repo).rows()) == "timeline: no synced events yet"
