"""Event-level matching of labeled vs predicted events (F29, spec §6 D5, §9).

Matching decides which predictions correspond to which ground-truth labels.
Labels are processed in time order; each label takes the *nearest* unmatched
prediction inside the tolerance window (tie → the earliest prediction;
``dt ≤ tolerance`` is inclusive). Unmatched predictions become false
positives, unmatched labels become false negatives.

The algorithm is deterministic greedy (stdlib only). The signature is stable,
so a M5 revision may swap in optimal bipartite (Hungarian) matching without
touching callers (D5).
"""

from __future__ import annotations

from dataclasses import dataclass

# [EXPERIMENTAL] Predicted vs labeled event counted as the same event if they
# start within this window. Why: starting point for sparse, seconds-scale
# events; tune with recorded data (F28/F29). Spec F29 §8.
K_EVENT_MATCH_TOLERANCE_S = 60

# [DERIVED] Identity unit conversion: the tolerance constant is in seconds,
# event timestamps are in milliseconds (DATASET_FORMAT §2).
_K_MS_PER_S = 1000


@dataclass(frozen=True)
class Event:
    """One event row: prediction or label (F29 D6 subset of the F28 format).

    ``duration_ms`` and ``confidence`` are carried through for reporting;
    matching uses only ``t_start_ms``.
    """

    t_start_ms: int
    duration_ms: int = 0
    confidence: float = 0.0


@dataclass(frozen=True)
class Match:
    """One label↔prediction pair. ``dt_s`` = prediction − label (positive = late)."""

    label_index: int
    prediction_index: int
    dt_s: float


@dataclass(frozen=True)
class MatchResult:
    """Outcome of :func:`match_events`; indices refer to the input sequences."""

    matches: tuple[Match, ...]
    false_positive_indices: tuple[int, ...]
    false_negative_indices: tuple[int, ...]

    @property
    def tp(self) -> int:
        return len(self.matches)

    @property
    def fp(self) -> int:
        return len(self.false_positive_indices)

    @property
    def fn(self) -> int:
        return len(self.false_negative_indices)

    @property
    def matched_dt_s(self) -> tuple[float, ...]:
        return tuple(match.dt_s for match in self.matches)


def match_events(
    labels: list[Event],
    predictions: list[Event],
    tolerance_s: float = K_EVENT_MATCH_TOLERANCE_S,
) -> MatchResult:
    """Greedy nearest-label matching (D5); pure and order-independent.

    Input order does not matter: labels and predictions are processed in
    ascending ``t_start_ms`` (ties broken by input index); the returned
    indices always refer to the caller's input sequences.
    """
    tol_ms = int(round(tolerance_s * _K_MS_PER_S))
    label_order = sorted(range(len(labels)), key=lambda i: (labels[i].t_start_ms, i))
    pred_order = sorted(range(len(predictions)), key=lambda i: (predictions[i].t_start_ms, i))

    used_predictions: set[int] = set()
    matched_labels: set[int] = set()
    matches: list[Match] = []
    for label_index in label_order:
        label_ms = labels[label_index].t_start_ms
        best: tuple[tuple[int, int, int], int, int] | None = None
        for pred_index in pred_order:
            if pred_index in used_predictions:
                continue
            dt_ms = predictions[pred_index].t_start_ms - label_ms
            if abs(dt_ms) > tol_ms:
                continue
            # Nearest wins; tie → earliest prediction, then lowest input index.
            key = (abs(dt_ms), predictions[pred_index].t_start_ms, pred_index)
            if best is None or key < best[0]:
                best = (key, pred_index, dt_ms)
        if best is not None:
            used_predictions.add(best[1])
            matched_labels.add(label_index)
            matches.append(
                Match(
                    label_index=label_index,
                    prediction_index=best[1],
                    dt_s=best[2] / _K_MS_PER_S,
                )
            )

    false_positives = tuple(i for i in range(len(predictions)) if i not in used_predictions)
    false_negatives = tuple(i for i in range(len(labels)) if i not in matched_labels)
    return MatchResult(tuple(matches), false_positives, false_negatives)
