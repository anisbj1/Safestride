"""Metric-function tests on hand-computed toy data (F29 spec §9 "Metrics")."""

import pytest
from metrics import (
    add_counts,
    event_level_metrics,
    f1,
    false_alerts_per_day,
    latency_stats,
    missed_event_rate,
    precision,
    recall,
    unlabeled_coverage,
)


def test_precision_recall_f1_two_thirds() -> None:
    # Spec §9: TP=2 FP=1 FN=1 → precision = recall = F1 = 2/3.
    p = precision(2, 1)
    r = recall(2, 1)
    assert p == pytest.approx(2 / 3)
    assert r == pytest.approx(2 / 3)
    assert f1(p, r) == pytest.approx(2 / 3)


def test_precision_none_with_no_predictions() -> None:
    assert precision(0, 0) is None  # D4: unknown, not 0.0


def test_recall_none_with_no_labels() -> None:
    assert recall(0, 0) is None  # D4: unknown, not 0.0


def test_f1_none_when_an_input_is_undefined() -> None:
    assert f1(None, 1.0) is None
    assert f1(1.0, None) is None
    assert f1(None, None) is None


def test_f1_zero_when_both_rates_defined_and_zero() -> None:
    assert f1(0.0, 0.0) == 0.0  # a real (bad) score, not an undefined one


def test_false_alerts_per_day() -> None:
    # Spec §9: FP=1 over 7200 s → 12.0 per day (7200 s = 1/12 day).
    assert false_alerts_per_day(1, 7200.0) == pytest.approx(12.0)
    assert false_alerts_per_day(0, 7200.0) == 0.0
    assert false_alerts_per_day(0, 0.0) is None  # unknown duration


def test_missed_event_rate() -> None:
    # Spec §9: FN=1 on 3 labels → 1/3.
    assert missed_event_rate(1, 3) == pytest.approx(1 / 3)
    assert missed_event_rate(0, 3) == 0.0
    assert missed_event_rate(0, 0) is None


def test_unlabeled_coverage() -> None:
    # Spec §9: 1800 s of 7200 s → 0.25.
    assert unlabeled_coverage(1800.0, 7200.0) == pytest.approx(0.25)
    assert unlabeled_coverage(0.0, 7200.0) == 0.0
    assert unlabeled_coverage(600.0, 0.0) is None


def test_latency_stats_on_matched_dt() -> None:
    # Spec §9: matched dt [+20, +20] → mean/median 20.0.
    stats = latency_stats([20.0, 20.0])
    assert stats.mean_s == pytest.approx(20.0)
    assert stats.median_s == pytest.approx(20.0)
    assert stats.min_s == 20.0
    assert stats.max_s == 20.0


def test_latency_stats_mixed_signs_and_order() -> None:
    stats = latency_stats([50.0, -20.0, 20.0, 30.0, 50.0])
    assert stats.mean_s == pytest.approx(26.0)
    assert stats.median_s == pytest.approx(30.0)
    assert stats.min_s == -20.0
    assert stats.max_s == 50.0


def test_latency_stats_empty_is_all_none() -> None:
    stats = latency_stats([])
    assert (stats.mean_s, stats.median_s, stats.min_s, stats.max_s) == (None, None, None, None)


def test_event_level_metrics_block() -> None:
    block = event_level_metrics(
        tp=2, fp=1, fn=1, matched_dt_s=[20.0, 20.0], total_duration_s=7200.0, unlabeled_s=1800.0
    )
    assert (block.tp, block.fp, block.fn) == (2, 1, 1)
    assert block.precision == pytest.approx(2 / 3)
    assert block.recall == pytest.approx(2 / 3)
    assert block.f1 == pytest.approx(2 / 3)
    assert block.false_alerts_per_day == pytest.approx(12.0)
    assert block.missed_event_rate == pytest.approx(1 / 3)
    assert block.latency.mean_s == pytest.approx(20.0)
    assert block.unlabeled_coverage == pytest.approx(0.25)


def test_event_level_metrics_undefined_stays_none() -> None:
    block = event_level_metrics(
        tp=0, fp=0, fn=0, matched_dt_s=[], total_duration_s=0.0, unlabeled_s=0.0
    )
    assert block.precision is None
    assert block.recall is None
    assert block.f1 is None
    assert block.false_alerts_per_day is None
    assert block.missed_event_rate is None
    assert block.unlabeled_coverage is None


def test_add_counts() -> None:
    assert add_counts((1, 2, 3), (4, 5, 6)) == (5, 7, 9)
    assert add_counts((0, 0, 0), (0, 0, 0)) == (0, 0, 0)
