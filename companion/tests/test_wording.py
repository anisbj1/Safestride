"""§9 test 13 — wording rules: "possible"/"suspected" + confidence, `unknown (#n)`,
`reason #N`, age labels, and no diagnostic/medical claim wording anywhere in
rendered output (spec §5, §14 — the project is not a medical device)."""

from __future__ import annotations

from companion.data.models import SyncStatus
from companion.domain import wording
from companion.features.live import LiveViewModel
from companion.features.sync_control import SyncControlViewModel
from companion.features.timeline import TimelineViewModel
from companion.sync_port import FakeSyncPort
from companion.ui import render
from conftest import run
from vectors import V1_LIVE_STATUS, make_event_bytes

WALL_KNOWN = 1_760_000_000


def test_detection_wording_is_possible_or_suspected_with_confidence():
    assert wording.detection_label(1, 0.72) == "possible drowsiness (confidence 0.72)"
    assert wording.detection_label(2, 0.8).startswith("suspected ")
    assert wording.detection_label(0, 0.55).startswith("possible ")
    assert wording.detection_label(4, 0.5) is None  # AlertRaised is not a detection


def test_confidence_formatting_is_two_decimals():
    assert wording.format_confidence(0.5) == "0.50"
    assert wording.format_confidence(1.0) == "1.00"


def test_unknown_enum_values_render_unknown_n():
    assert wording.format_enum(wording.ALERTNESS_NAMES, 3) == "Drowsy"
    assert wording.format_enum(wording.ALERTNESS_NAMES, 9) == "unknown (#9)"
    assert wording.format_enum(wording.EVENT_TYPE_NAMES, 12) == "unknown (#12)"


def test_unknown_reason_bits_render_reason_n():
    assert wording.format_reasons(0) == "none"
    assert wording.format_reasons(1 << 0) == "SustainedLowMotion"
    assert wording.format_reasons(1 << 17) == "ShortMotionBurst"
    assert wording.format_reasons(1 << 21) == "reason #21"


def test_age_labels_and_stale_marker():
    assert wording.age_text(500) == "0s ago"
    assert wording.age_text(3_000) == "3s ago"  # under kStaleLabelMs = 5000
    assert wording.age_text(7_000) == "7s ago (stale)"  # > kStaleLabelMs
    assert wording.age_text(120_000) == "2m ago (stale)"  # stale applies at any scale
    assert wording.age_text(2 * 60 * 60 * 1000) == "2h ago (stale)"


def test_battery_and_time_wording():
    assert wording.battery_text(None) == "unknown"
    assert wording.battery_text(87) == "87%"
    assert wording.event_time_text(0) == "unknown"  # never fabricated


def test_no_diagnostic_wording_in_any_rendered_output(repo, clock):
    """Full sweep of every render path with rich data: live + timeline + sync."""
    repo.upsert(
        "dev1",
        [
            make_event_bytes(
                seq=1, type=1, confidence=0.9, reasons=1 << 3, t_start_unix=WALL_KNOWN
            ),
            make_event_bytes(seq=2, type=2, confidence=0.4, t_start_unix=0),
            make_event_bytes(seq=3, type=0, confidence=0.6, t_start_unix=WALL_KNOWN),
            make_event_bytes(seq=4, type=5, t_start_unix=WALL_KNOWN),  # SensorFault
            make_event_bytes(seq=5, type=11, t_start_unix=WALL_KNOWN),  # Boot
            make_event_bytes(seq=6, type=1, reasons=1 << 20, t_start_unix=0),
        ],
    )
    timeline_text = render.render_timeline(TimelineViewModel(repo, device_id="dev1").rows())

    live = LiveViewModel(now_ms=clock.now_ms, sync_port=FakeSyncPort())
    live.on_live_status(V1_LIVE_STATUS)
    live.on_battery(bytes([87]))
    live.set_connected(True)
    live_text = render.render_live(live.snapshot())
    live.set_connected(False)
    live_text += render.render_live(live.snapshot())  # include the not-live banner

    device = FakeSyncPort(status=SyncStatus(linked=True, link_up=True, streaming=True, last_seq=6))
    sync_vm = SyncControlViewModel(
        repo, device, device_id="dev1", now_ms=clock.now_ms, mtu_size=185
    )
    device.set_record_sink(sync_vm.on_records)
    run(sync_vm.start())
    device.emit_records([make_event_bytes(seq=7, type=1, t_start_unix=WALL_KNOWN)])
    sync_text = render.render_sync(run(sync_vm.tick()))

    everything = f"{timeline_text}\n{live_text}\n{sync_text}".lower()
    for term in wording.BANNED_DIAGNOSTIC_TERMS:
        assert term not in everything, f"banned diagnostic term {term!r} in rendered output"
    # The mandated detection wording is present, not absent (spec §14 DoD).
    assert "possible drowsiness (confidence 0.90)" in timeline_text
