"""Event-level metric functions (F29, spec §1 responsibility, §9 test plan).

Pure "counts in → values out" functions (D2: fold generation lives in
``split.py``, report shaping in ``report.py``). Undefined metrics — a ratio
whose denominator is empty — return ``None`` (D4): "unknown is a valid
output" (00 §7.5); the report renders them as ``null`` / ``n/a``. Micro
aggregates are computed from summed TP/FP/FN counts, so they stay defined.

No metric here is "accuracy"; the skeleton reports precision/recall/F1,
false alerts per day, missed-event rate and latency (spec §5: never accuracy
alone).
"""

from __future__ import annotations

import statistics
from collections.abc import Sequence
from dataclasses import dataclass

# [DERIVED] Definition of a day for the false-alert rate; the spec expresses
# the rate "per day" (F29 §1), so the conversion is fixed, not a tuning knob.
_K_S_PER_DAY = 24 * 60 * 60


def precision(tp: int, fp: int) -> float | None:
    """tp / (tp + fp); ``None`` when nothing was predicted (D4)."""
    total = tp + fp
    return tp / total if total > 0 else None


def recall(tp: int, fn: int) -> float | None:
    """tp / (tp + fn); ``None`` when there are no labels (D4)."""
    total = tp + fn
    return tp / total if total > 0 else None


def f1(precision_v: float | None, recall_v: float | None) -> float | None:
    """Harmonic mean of precision and recall; ``None`` if either is undefined."""
    if precision_v is None or recall_v is None:
        return None
    if precision_v + recall_v == 0.0:
        return 0.0  # both defined and zero — a real, if bad, score
    return 2.0 * precision_v * recall_v / (precision_v + recall_v)


def false_alerts_per_day(fp: int, total_duration_s: float) -> float | None:
    """False predictions per day of recorded data; ``None`` if duration unknown."""
    if total_duration_s <= 0:
        return None
    return fp / (total_duration_s / _K_S_PER_DAY)


def missed_event_rate(fn: int, n_labels: int) -> float | None:
    """fn / labels; ``None`` when there are no labels (D4)."""
    return fn / n_labels if n_labels > 0 else None


def unlabeled_coverage(unlabeled_s: float, total_duration_s: float) -> float | None:
    """Share of recorded time with no ground truth (spec §10); ``None`` if unknown."""
    if total_duration_s <= 0:
        return None
    return unlabeled_s / total_duration_s


@dataclass(frozen=True)
class LatencyStats:
    """Detection latency in seconds (prediction − label, positive = late)."""

    mean_s: float | None
    median_s: float | None
    min_s: float | None
    max_s: float | None


def latency_stats(matched_dt_s: Sequence[float]) -> LatencyStats:
    """Latency summary over matched pairs; all-``None`` when nothing matched."""
    if not matched_dt_s:
        return LatencyStats(None, None, None, None)
    return LatencyStats(
        mean_s=statistics.fmean(matched_dt_s),
        median_s=statistics.median(matched_dt_s),
        min_s=min(matched_dt_s),
        max_s=max(matched_dt_s),
    )


@dataclass(frozen=True)
class EventLevelMetrics:
    """The full event-level metric block (spec §1)."""

    tp: int
    fp: int
    fn: int
    precision: float | None
    recall: float | None
    f1: float | None
    false_alerts_per_day: float | None
    missed_event_rate: float | None
    latency: LatencyStats
    unlabeled_coverage: float | None


def event_level_metrics(
    tp: int,
    fp: int,
    fn: int,
    matched_dt_s: Sequence[float],
    total_duration_s: float,
    unlabeled_s: float,
) -> EventLevelMetrics:
    """Assemble every event-level metric from counts (single source of truth)."""
    precision_v = precision(tp, fp)
    recall_v = recall(tp, fn)
    return EventLevelMetrics(
        tp=tp,
        fp=fp,
        fn=fn,
        precision=precision_v,
        recall=recall_v,
        f1=f1(precision_v, recall_v),
        false_alerts_per_day=false_alerts_per_day(fp, total_duration_s),
        missed_event_rate=missed_event_rate(fn, tp + fn),
        latency=latency_stats(matched_dt_s),
        unlabeled_coverage=unlabeled_coverage(unlabeled_s, total_duration_s),
    )


def add_counts(a: tuple[int, int, int], b: tuple[int, int, int]) -> tuple[int, int, int]:
    """Sum two (tp, fp, fn) triples for micro aggregation."""
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
