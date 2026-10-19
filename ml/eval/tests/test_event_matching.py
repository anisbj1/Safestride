"""Event-matching tests on hand-computed toy data (F29 spec §9 "Matching")."""

import event_matching
import pytest
from event_matching import K_EVENT_MATCH_TOLERANCE_S, Event, match_events


def _events_at(seconds: list[float], confidence: float = 0.9) -> list[Event]:
    return [Event(t_start_ms=int(s * 1000), duration_ms=0, confidence=confidence) for s in seconds]


def test_case_a_basic_matching() -> None:
    # Spec §9 case A: labels [100, 500, 900], preds [120, 520, 1300], tol 60.
    result = match_events(_events_at([100, 500, 900]), _events_at([120, 520, 1300], 0.5))
    assert result.tp == 2
    assert result.fp == 1
    assert result.fn == 1
    matched = {(m.label_index, m.prediction_index) for m in result.matches}
    assert matched == {(0, 0), (1, 1)}
    assert result.false_positive_indices == (2,)  # pred 1300
    assert result.false_negative_indices == (2,)  # label 900
    assert result.matched_dt_s == pytest.approx((20.0, 20.0))


def test_double_detections_label_takes_nearest() -> None:
    # Spec §9: labels [300], preds [310, 350] → label takes 310 (dt 10); 350 is FP.
    result = match_events(_events_at([300]), _events_at([310, 350], 0.5))
    assert result.tp == 1
    assert result.fp == 1
    assert result.fn == 0
    assert result.matches[0].prediction_index == 0
    assert result.matches[0].dt_s == pytest.approx(10.0)
    assert result.false_positive_indices == (1,)


def test_one_prediction_between_two_labels_earliest_label_wins() -> None:
    # Spec §9: labels [300, 330], preds [315] → tie |dt|=15, earliest label wins.
    result = match_events(_events_at([300, 330]), _events_at([315], 0.5))
    assert result.tp == 1
    assert result.fn == 1
    assert result.fp == 0
    assert result.matches[0].label_index == 0
    assert result.matches[0].dt_s == pytest.approx(15.0)
    assert result.false_negative_indices == (1,)


def test_boundary_dt_exactly_tolerance_matches() -> None:
    result = match_events(_events_at([100]), _events_at([160], 0.5))
    assert result.tp == 1
    assert result.matches[0].dt_s == pytest.approx(60.0)


def test_boundary_just_over_tolerance_does_not_match() -> None:
    result = match_events(
        [Event(t_start_ms=0)], [Event(t_start_ms=60_001, confidence=0.5)]
    )
    assert result.tp == 0
    assert result.fp == 1
    assert result.fn == 1


def test_boundary_negative_dt_exactly_tolerance_matches() -> None:
    # Symmetric to test_boundary_dt_exactly_tolerance_matches: prediction 60 s
    # *before* the label is dt = −60.0; the tolerance is |dt|, so it matches.
    result = match_events(_events_at([100]), _events_at([40], 0.5))
    assert result.tp == 1
    assert result.matches[0].dt_s == pytest.approx(-60.0)


def test_boundary_negative_dt_just_over_tolerance_does_not_match() -> None:
    # Symmetric to test_boundary_just_over_tolerance_does_not_match.
    result = match_events(
        [Event(t_start_ms=60_001)], [Event(t_start_ms=0, confidence=0.5)]
    )
    assert result.tp == 0
    assert result.fp == 1
    assert result.fn == 1


def test_negative_dt_early_detection_matches() -> None:
    result = match_events(_events_at([500]), _events_at([480], 0.5))
    assert result.tp == 1
    assert result.matches[0].dt_s == pytest.approx(-20.0)


def test_input_order_does_not_change_the_result() -> None:
    labels = _events_at([900, 100, 500])
    predictions = _events_at([1300, 120, 520], 0.5)
    shuffled_labels = _events_at([500, 900, 100])
    shuffled_predictions = _events_at([520, 1300, 120], 0.5)
    expected_times = sorted(
        (labels[m.label_index].t_start_ms, predictions[m.prediction_index].t_start_ms)
        for m in match_events(labels, predictions).matches
    )
    result = match_events(shuffled_labels, shuffled_predictions)
    actual_times = sorted(
        (
            shuffled_labels[m.label_index].t_start_ms,
            shuffled_predictions[m.prediction_index].t_start_ms,
        )
        for m in result.matches
    )
    assert actual_times == expected_times


def test_empty_inputs() -> None:
    result = match_events([], [])
    assert (result.tp, result.fp, result.fn) == (0, 0, 0)


def test_no_labels_all_predictions_are_false_positives() -> None:
    result = match_events([], _events_at([10, 20], 0.5))
    assert (result.tp, result.fp, result.fn) == (0, 2, 0)


def test_no_predictions_all_labels_are_false_negatives() -> None:
    result = match_events(_events_at([10, 20]), [])
    assert (result.tp, result.fp, result.fn) == (0, 0, 2)


def test_default_tolerance_is_the_spec_constant() -> None:
    assert K_EVENT_MATCH_TOLERANCE_S == 60  # spec §8
    tight = match_events(_events_at([100]), _events_at([105], 0.5), tolerance_s=2)
    assert tight.tp == 0  # dt 5 s > 2 s tolerance
    exact = match_events(_events_at([100]), _events_at([101], 0.5), tolerance_s=1)
    assert exact.tp == 1  # dt 1 s == tolerance, inclusive


def test_event_is_immutable_value_type() -> None:
    event = Event(t_start_ms=1)
    with pytest.raises(AttributeError):
        event.t_start_ms = 2  # type: ignore[misc]
    assert event_matching.Event is Event
