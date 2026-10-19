"""Pure builder tests (spec §9 items 22-28): ordering, unknown bits, empty mask,
detection wording + confidence, unsupported schema, banned-terms sweep. No DB."""

from __future__ import annotations

import pytest
from companion.data.models import EVENT_SCHEMA_VERSION, EventRecord
from companion.domain import wording
from companion.features.explain import builder
from companion.features.explain.catalog import REASON_EXPLANATIONS


def make_record(**overrides) -> EventRecord:
    """Contract-shaped record for pure tests (raw blob is F21's authority; the
    builder never reads it)."""
    fields: dict[str, object] = dict(
        t_start_ms=1_000_000,
        seq=1,
        boot_id=7,
        duration_ms=30_000,
        t_start_unix=0,
        confidence=0.5,
        reasons=0,
        payload=b"\x00" * 8,
        quality=0,
        type=1,
        label=0,
        context=0,
        alertness_at_start=0,
        detector_id=0,
        schema_version=EVENT_SCHEMA_VERSION,
        raw=b"\x00" * 48,
        wall_unix=0,
    )
    fields.update(overrides)
    return EventRecord(**fields)  # type: ignore[arg-type]


def test_single_known_bit_roundtrips():
    """Spec §9 item 22: every catalog code round-trips bit -> name + sentence."""
    for bit, sentence in REASON_EXPLANATIONS.items():
        explanation = builder.build_explanation(make_record(reasons=1 << bit))
        assert len(explanation.reasons) == 1
        item = explanation.reasons[0]
        assert item.bit_index == bit
        assert item.name == wording.REASON_NAMES[bit]
        assert item.explanation == sentence
        assert item.text == f"{item.name}: {sentence}"


def test_multi_bit_ascending_and_complete():
    """Spec §9 item 23: ascending bit index, ALL set bits (no 3-reason cap, D1/D4)."""
    mask = (1 << 17) | (1 << 0) | (1 << 5)
    explanation = builder.build_explanation(make_record(reasons=mask))
    assert [item.bit_index for item in explanation.reasons] == [0, 5, 17]
    full = builder.build_explanation(make_record(reasons=(1 << len(wording.REASON_NAMES)) - 1))
    assert [item.bit_index for item in full.reasons] == list(range(len(wording.REASON_NAMES)))


def test_unknown_bits_surface_as_reason_n():
    """Spec §9 item 24: unknown bits at their own positions, mixed with known bits."""
    mask = (1 << 0) | (1 << 20) | (1 << 31)
    explanation = builder.build_explanation(make_record(reasons=mask))
    assert [item.bit_index for item in explanation.reasons] == [0, 20, 31]
    assert explanation.reasons[1].text == f"{wording.UNKNOWN_REASON_PREFIX}20"
    assert explanation.reasons[2].text == f"{wording.UNKNOWN_REASON_PREFIX}31"
    assert explanation.reasons[1].name is None and explanation.reasons[1].explanation is None
    assert explanation.reasons[0].name == wording.REASON_NAMES[0]


def test_empty_mask_is_valid_and_not_backfilled():
    """Spec §9 item 25: mask == 0 -> no items, rendered 'none'; nothing guessed."""
    explanation = builder.build_explanation(make_record(reasons=0))
    assert explanation.reasons == ()
    assert "reasons: none" in builder.render_explanation(explanation)


def test_detection_wording_with_confidence():
    """Spec §9 item 26: detections possible/suspected + confidence value; other
    types plain; confidence always rendered as a value, never as certainty."""
    drowsy = builder.build_explanation(make_record(type=1, confidence=0.8))
    assert drowsy.label == "possible drowsiness (confidence 0.80)"
    assert drowsy.confidence_text == "0.80"
    cataplexy = builder.build_explanation(make_record(type=2, confidence=0.42))
    assert cataplexy.label == "suspected cataplexy-like event (confidence 0.42)"
    fault = builder.build_explanation(make_record(type=5, confidence=0.0))
    assert fault.label == wording.format_enum(wording.EVENT_TYPE_NAMES, 5)


def test_unsupported_schema_renders_no_semantics():
    """Spec §9 item 27: unknown schema -> flag only, no reasons/label/confidence."""
    explanation = builder.build_explanation(make_record(schema_version=99, reasons=(1 << 3) | 1))
    assert explanation.schema_supported is False
    assert explanation.reasons == ()
    assert explanation.time_known is False
    assert explanation.confidence_text == ""
    assert "unsupported schema #99" in explanation.label


def test_banned_terms_never_reach_rendered_output():
    """Spec §9 item 28: full rendered output (label + all reasons) for every
    EventType x an all-bits mask contains no banned diagnostic term."""
    all_bits = (1 << builder.EXPLAIN_MAX_REASON_BITS) - 1
    for event_type in range(len(wording.EVENT_TYPE_NAMES)):
        for confidence in (0.0, 0.5, 1.0):
            record = make_record(type=event_type, confidence=confidence, reasons=all_bits)
            rendered = builder.render_explanation(builder.build_explanation(record)).lower()
            for term in wording.BANNED_DIAGNOSTIC_TERMS:
                assert term not in rendered, f"type {event_type}: banned term {term!r}"


@pytest.mark.parametrize("event_type", sorted(wording.DETECTION_WORDS))
def test_detection_types_always_carry_confidence(event_type: int):
    """Spec §14 DoD companion side: a detection label is never rendered without
    its confidence value (wording rule)."""
    for confidence in (0.0, 0.37, 1.0):
        explanation = builder.build_explanation(make_record(type=event_type, confidence=confidence))
        assert f"(confidence {confidence:.2f})" in explanation.label
