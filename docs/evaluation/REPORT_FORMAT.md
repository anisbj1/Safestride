# Evaluation report format (F29, schema v1)

> Owner: **F29**. Output contract of `ml/eval/run_eval.py` (`report.json` +
> `report.md`). Consumed by humans now and by comparison tooling in M5.
> Changes to any key, section order or definition here are report-shape
> changes: bump `K_REPORT_SCHEMA_VERSION` (spec §8), regenerate the committed
> example `docs/evaluation/example/` in the same commit, and keep the drift
> guard green.

## 1. What the report is

An **event-level evaluation** of one detector over one or more F28 session
directories, using leave-one-session-out folds (one fold per session; the
session is both the test unit and the evaluation unit — the skeleton has no
training step, M5 adds it behind the same fold shape). Detections are
possible/suspected events with a confidence value; this is evaluation output
from an academic prototype — **not a diagnosis, not a medical device** (00 §1).
The report deliberately contains **no `accuracy` key** (spec §5): a
near-constant detector would score high on it while being useless.

## 2. Inputs

- **Sessions** — F28 session directories (`docs/contracts/DATASET_FORMAT.md`,
  schema v1): `session.json` (required; unknown `schema_version` → exit 4)
  + `events.csv` (labels).
- **Predictions** — `<session>/predictions_<detector_id>.csv` with header
  `t_start_ms,duration_ms,confidence` (F29 D6: a strict subset of the F28
  `events.csv` columns; extra columns are tolerated and ignored — F29 parses
  only the columns it needs and never re-implements the full grammar).
- **Labels** — `events.csv` rows by `label`:
  - `Confirmed` → positive ground-truth label (matched against predictions),
  - `Rejected` → ground truth "not an event": never matched, never an FN, but
    it **does not forgive** a prediction — a firing near a `Rejected` label is
    still a false positive, and the FP detail row shows the distance to it,
  - `Unlabeled` → unknown truth (spec §10): excluded from matching entirely;
    its `duration_ms` only feeds the unlabeled-coverage metric,
  - unknown label names / malformed numeric fields → row counted malformed,
    never repaired or guessed (DATASET_FORMAT §4).

## 3. Definitions

| Term | Definition |
|---|---|
| match | a `Confirmed` label and a prediction within `event_match_tolerance_s` (default `K_EVENT_MATCH_TOLERANCE_S = 60`, `EXPERIMENTAL`, spec §8). Greedy, deterministic: labels in time order take the nearest unmatched prediction; tie → earliest prediction; `dt ≤ tolerance` inclusive (D5) |
| `dt` | `prediction.t_start_ms − label.t_start_ms`; **positive = late**. Used for latency and both FP/FN detail columns |
| tp / fp / fn | matched pairs / unmatched predictions / unmatched `Confirmed` labels |
| precision | `tp / (tp + fp)`; `null` (markdown `n/a`) when nothing was predicted (D4) |
| recall | `tp / (tp + fn)`; `null` when there are no labels (D4) |
| f1 | harmonic mean of precision and recall; `null` if either is undefined; `0.0` when both are defined and zero |
| false_alerts_per_day | `fp / (total_duration_s / 86400)`; `null` when duration is unknown |
| missed_event_rate | `fn / (tp + fn)`; `null` when there are no labels |
| latency | mean/median/min/max of the matched dts (seconds); all `null` when nothing matched |
| unlabeled_coverage | `Σ Unlabeled duration_ms / total_duration_s`; the share of recorded time with no ground truth. Unlabeled windows are assumed disjoint; coverage is not clamped (overlapping windows would exceed 1.0 — a data bug surfaced, not hidden) |
| total_duration_s | `ended_utc − started_utc` from `session.json`; fallback: last row timestamp in `features.csv`/`events.csv` |

Undefined metrics are **never silently 0** (D4, 00 §7.5): JSON `null`,
markdown `n/a`. Micro aggregates are computed from summed TP/FP/FN counts, so
they stay defined whenever any fold had labels or predictions.

## 4. JSON schema v1 (`report.json`)

Top level (sorted keys, 2-space indent, LF, trailing newline — byte-stable):

| Key | Type | Meaning |
|---|---|---|
| `schema_version` | int | `1` (`K_REPORT_SCHEMA_VERSION`) |
| `generated_by` | str | static tool id — **no timestamps**, so regeneration is byte-identical |
| `disclaimer` | str | the possible/suspected + not-a-medical-device wording |
| `config` | object | `detector_id`, `event_match_tolerance_s`, `split` (`"leave-one-session-out"`) |
| `dataset` | object | `session_ids[]`, `total_duration_s`, `unlabeled_coverage`, `notes` |
| `dataset.notes` | object | `raw_drops` / `line_drops` / `malformed_rows` totals (a degraded session is never presented as a clean result), `missing_prediction_files[]` (`"session:detector"` — a session without a predictions file is evaluated as 0 predictions and warned, never silent), `unlabeled_s_total`, `per_session{}` (`duration_s`, `unlabeled_s`, `unlabeled_coverage`, drop counters) |
| `folds[]` | list | one entry per session: `fold_index`, `test_session`, `detector_id`, then the metric block |
| `aggregate` | object | metric block over all sessions |
| `comparison[]` | list | one row per `detector_id` with the metric block minus the FP/FN detail lists (M5: heuristic-vs-model rows land here) |

**Metric block** (shared by folds, aggregate, comparison):
`tp`, `fp`, `fn`, `precision`, `recall`, `f1`, `false_alerts_per_day`,
`missed_event_rate`, `latency{mean_s,median_s,min_s,max_s}`,
plus `false_positives[]` / `false_negatives[]` in folds and aggregate.

**FP detail row**: `session`, `t_start_ms`, `confidence`,
`nearest_label_dt_s` (dt to the nearest labeled event of any status except
`Unlabeled`; `null` when the dataset has no label at all).
**FN detail row**: `session`, `t_start_ms`, `confidence`,
`nearest_prediction_dt_s` (dt to the nearest prediction; `null` when the
detector never fired).

## 5. Markdown (`report.md`)

Fixed section order (title + disclaimer, then): `Configuration` · `Dataset`
(incl. data-quality notes and a warning line for missing prediction files) ·
`Event-level metrics` (aggregate table first, then one subsection per fold) ·
**False positives** · **False negatives** (both always rendered; `none` when
empty — F29 spec §15) · `Detection latency` · `Unlabeled coverage` · `Model comparison`.
Rates render with 3 decimals, counts as integers, undefined as `n/a`.

## 6. Exit codes (mirror `tools/replay/compare.py`, DATASET_FORMAT §4)

| Code | Constant | When |
|---|---|---|
| 0 | `K_EXIT_OK` | report written |
| 2 | `K_EXIT_INPUT_ERROR` | missing/unreadable `session.json`, malformed required columns or prediction rows, no sessions/predictions given, fewer than 2 sessions or duplicate session ids (leave-one-session-out cannot fold those), non-positive or non-finite `--tolerance-s` (argparse usage error) |
| 4 | `K_EXIT_SCHEMA_VERSION` | `session.json` declares an unknown `schema_version` |

## 7. Determinism and the drift guard

The committed example `docs/evaluation/example/report.{json,md}` is generated
from the canonical two-session synthetic set
(`ml/eval/tests/fixtures/synthetic.py::write_canonical_sessions`, no RNG).
`test_end_to_end.py::test_example_report_matches_committed_files` regenerates
the whole pipeline in a temp dir and compares content against the committed
files (universal-newline read, so CRLF checkouts — repo backlog item 9 — pass
unchanged). Mutating the fixture writer, the stub detector, matching, metrics
or report shaping without regenerating the example fails the guard.
