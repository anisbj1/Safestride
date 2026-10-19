# F29 event-level evaluation report — stub_stillness

> Detections are possible/suspected events with a confidence value. This report is evaluation output from an academic prototype — not a diagnosis, not a medical device.

## Configuration

- detector_id: stub_stillness
- event match tolerance: 60.000 s
- split: leave-one-session-out
- report schema version: 1

## Dataset

| session | duration_s | unlabeled_coverage |
|---|---|---|
| synthetic_sA | 7200 | 0.083 |
| synthetic_sB | 5400 | 0.000 |

- total duration: 12600 s
- unlabeled coverage (aggregate): 0.048
- data-quality notes: raw_drops=0, line_drops=0, malformed_rows=0

## Event-level metrics

### Aggregate

| tp | fp | fn | precision | recall | f1 | false_alerts_per_day | missed_event_rate |
|---|---|---|---|---|---|---|---|
| 5 | 1 | 0 | 0.833 | 1.000 | 0.909 | 6.857 | 0.000 |

### Fold 1 — test session synthetic_sA (detector stub_stillness)

| tp | fp | fn | precision | recall | f1 | false_alerts_per_day | missed_event_rate |
|---|---|---|---|---|---|---|---|
| 3 | 1 | 0 | 0.750 | 1.000 | 0.857 | 12.000 | 0.000 |

### Fold 2 — test session synthetic_sB (detector stub_stillness)

| tp | fp | fn | precision | recall | f1 | false_alerts_per_day | missed_event_rate |
|---|---|---|---|---|---|---|---|
| 2 | 0 | 0 | 1.000 | 1.000 | 1.000 | 0.000 | 0.000 |

## False positives

| session | t_start_ms | confidence | nearest_label_dt_s |
|---|---|---|---|
| synthetic_sA | 2410000 | 0.500 | 10.000 |

## False negatives

none

## Detection latency

| scope | mean_s | median_s | min_s | max_s |
|---|---|---|---|---|
| aggregate | 26.000 | 30.000 | -20.000 | 50.000 |
| synthetic_sA | 16.667 | 20.000 | -20.000 | 50.000 |
| synthetic_sB | 40.000 | 40.000 | 30.000 | 50.000 |

## Unlabeled coverage

| session | unlabeled_s | coverage |
|---|---|---|
| synthetic_sA | 600 | 0.083 |
| synthetic_sB | 0 | 0.000 |
| aggregate | 600 | 0.048 |

## Model comparison

| detector_id | tp | fp | fn | precision | recall | f1 | false_alerts_per_day | missed_event_rate |
|---|---|---|---|---|---|---|---|---|
| stub_stillness | 5 | 1 | 0 | 0.833 | 1.000 | 0.909 | 6.857 | 0.000 |
