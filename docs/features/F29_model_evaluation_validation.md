# F29 — Model evaluation & validation

> Track **B** · Milestone **M2 (skeleton) → M5** · Runs on **ml** · Contract changes only via change request.

## 1. Responsibility
Evaluation workflow for every model/heuristic: window-level and **event-level** metrics, false-alert rate per day, missed-event rate, detection latency, confusion matrices, resource usage — with false positives and false negatives analyzed separately.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- Session datasets (F28 format), labels (F14/manual), predictions from replay runs

**Outputs (produced contracts)**
- Metric reports (markdown/JSON), plots, model comparison tables

## 4. Dependencies
- Data + `tools/replay` outputs only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Train models.
- Report accuracy alone.
- Split train/test randomly within a session (leakage).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Validation split**
- Simplest: Random window split.
- Professional/scalable: Leave-one-session-out (and leave-one-subject-out when available).
- Trade-offs: Fewer training samples per fold.
- **Choice:** Leave-one-session-out — random splits leak temporal correlation.

**D2. Where the split logic lives**
- Simplest: fold generation inside `metrics.py`.
- Professional/scalable: dedicated `ml/eval/split.py`.
- Trade-offs: one file more vs. single-purpose modules (§7 spirit).
- **Choice:** `split.py` — generating folds is not a metric; keeps `metrics.py` pure counts-in/values-out functions.

**D3. Synthetic fixtures**
- Simplest: check in hand-written CSV session directories.
- Professional/scalable: a deterministic, seed-free fixture builder (`tests/fixtures/synthetic.py`) that writes F28-format session dirs at test time; commit only the *generated example report* under `docs/evaluation/example/` with a drift-guard test (regenerate → byte-compare — the F28 CR-012 lesson).
- Trade-offs: generated fixtures are one source of truth but not browsable as files; checked-in CSVs drift from the writer.
- **Choice:** generator + committed, drift-guarded example report.

**D4. Undefined metric values** (precision with no predictions, recall with no labels)
- Simplest: return `0.0`.
- Professional/scalable: return `None` → JSON `null`, markdown `n/a`.
- **Choice:** `None` — "unknown is a valid output" (00 §7.5); never fabricate confidence from absent data. Micro-aggregates are computed from summed TP/FP/FN counts, so they stay defined.

**D5. Event-matching algorithm**
- Simplest: deterministic greedy — labels in time order each take the nearest unmatched prediction within tolerance (tie → earliest prediction; `dt ≤ tolerance` inclusive).
- Professional/scalable: optimal bipartite assignment (Hungarian) — a dependency or ~100 extra lines for negligible gain on sparse events.
- Trade-offs: greedy can be suboptimal when events cluster; the function signature is stable, so M5 may swap the algorithm behind it.
- **Choice:** greedy, stdlib-only, fully pinned by edge-case tests (§9).

**D6. Predictions input format** (consumed contract, not owned)
- Simplest: free-form per-detector file.
- Professional: a strict subset of the F28 `events.csv` columns in `<session>/predictions_<detector_id>.csv`.
- **Choice:** subset `t_start_ms,duration_ms,confidence` — additive columns later without breaking the skeleton. F29 parses only the columns it needs and never re-implements the full DATASET_FORMAT grammar (extra columns tolerated).

## 7. Module structure
- `ml/eval/metrics.py` — event-level metric functions, pure (counts in → values out)
- `ml/eval/event_matching.py` — `Event`, `MatchResult`, `match_events()` using `K_EVENT_MATCH_TOLERANCE_S`
- `ml/eval/split.py` — `leave_one_session_out()` fold generation (D1, D2)
- `ml/eval/report.py` — JSON report model (`to_json`) + markdown renderer (`render_markdown`); false positives and false negatives are always separate sections (§5)
- `ml/eval/run_eval.py` — headless CLI harness: session dirs + `predictions_<detector_id>.csv` → `report.json` + `report.md`
- `ml/eval/tests/` — `conftest.py` (sys.path, same convention as `ml/common/tests`), `test_metrics.py`, `test_event_matching.py`, `test_split.py`, `test_report.py`, `test_end_to_end.py`, `fixtures/synthetic.py` (deterministic F28-format session builder + trivial stillness-threshold prediction stub)
- `docs/evaluation/REPORT_FORMAT.md` — the report contract (JSON schema v1, markdown section order)
- `docs/evaluation/example/` — committed synthetic example `report.json` / `report.md`, drift-guarded (D3)

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `K_EVENT_MATCH_TOLERANCE_S` | 60 | `EXPERIMENTAL` | Predicted vs labeled event counted as the same if within this window |
| `K_REPORT_SCHEMA_VERSION` | 1 | `DERIVED` | Versioned output contract; bump on any report shape change |
| `K_EXIT_OK` / `K_EXIT_INPUT_ERROR` / `K_EXIT_SCHEMA_VERSION` | 0 / 2 / 4 | `DERIVED` | Mirror `tools/replay/compare.py` exit codes per DATASET_FORMAT §4 so host tools fail identically |
| `K_STUB_STILLNESS_THRESHOLD_MS` | 45000 | `EXPERIMENTAL` | Test-fixture stub detector fires above this `stillness_ms`; arbitrary but named and pinned by tests |
| `K_STUB_REFRACTORY_S` | 120 | `EXPERIMENTAL` | Test-fixture stub suppresses re-fires within this window so each synthetic burst yields one prediction |

Python constants follow PEP 8 `SCREAMING_SNAKE_CASE` (as used consistently in `ml/eval` and `ml/common`); earlier doc drafts wrote firmware-style `kCamelCase` — the docs were reconciled to the code names in fix-loop-2, no code was renamed.

No other numeric values are introduced by the skeleton (latency stats are plain mean/median/min/max; the synthetic fixture uses hand-written timelines, no RNG). Fixture-internal values (burst stillness/length, label confidence, the definitional seconds-per-day factor) exist only inside `tests/fixtures/synthetic.py` / `metrics.py`; each is named and tagged there and pins test data rather than tuning a knob.

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Toy data with hand-computed values (all run under `python -m pytest`, no hardware):
- **Matching** (`test_event_matching.py`):
  - Case A: labels `[100, 500, 900]`, preds `[120, 520, 1300]`, tol 60 → matches (100↔120, 500↔520), FN `900`, FP `1300`; TP=2 FP=1 FN=1.
  - Double detections: labels `[300]`, preds `[310, 350]` → label takes nearest (`310`, dt 10); `350` is FP. TP=1 FP=1 FN=0.
  - One pred between two labels: labels `[300, 330]`, preds `[315]` → tie `|dt|=15`, earliest label wins; `330` FN. TP=1 FN=1 FP=0.
  - Boundary: `dt = 60.0` matches, `dt = 60.001` does not (inclusive tolerance).
- **Metrics** (`test_metrics.py`): precision = recall = F1 = 2/3 on TP=2 FP=1 FN=1; false-alerts-per-day = 12.0 for FP=1 over 7200 s; missed-event rate = 1/3 on 3 labels; latency mean/median 20.0 for matched dt `[+20, +20]` (dt = prediction − label, positive = late); unlabeled coverage = 0.25 for 1800 s of 7200 s; undefined denominators → `None` (D4).
- **Split** (`test_split.py`): 2 synthetic sessions → exactly 2 folds, every session is the test session once, `train ∩ test = ∅` (leakage guard); a single session → `ValueError`.
- **Report** (`test_report.py`): JSON round-trips (`json.loads(to_json(r)) == r`); two renders byte-equal (determinism); markdown contains separate `False positives` and `False negatives` sections, rendered even when empty ("none"); undefined metrics render as `n/a`; skeleton output contains no `accuracy` key anywhere (§5).
- **End-to-end** (`test_end_to_end.py`): two synthetic F28-format sessions written by `fixtures/synthetic.py`, predictions produced by the trivial stillness-threshold stub over synthetic `features.csv` motion rows. Hand-computed expectation: sA (7200 s, labels `[1200, 3600, 5800]`, one `Rejected` label at 2400, 600 s unlabeled) → stub fires at `[1220, 3580, 5850, 2410]` → TP=3 FP=1 FN=0; sB (5400 s, labels `[1000, 4000]`) → fires `[1030, 4050]` → TP=2 FP=0 FN=0. Aggregate: TP=5 FP=1 FN=0, precision 5/6, recall 1.0, F1 10/11, latency mean 26.0 / median 30.0 / min −20 / max 50, false-alerts-per-day ≈ 6.857, missed-event rate 0.0, unlabeled coverage ≈ 0.0476. Assert every value exactly.
- **Example drift guard**: regenerate the committed `docs/evaluation/example/report.json`/`.md` and byte-compare (muting the generator alone must fail this test).

## 10. Failure behavior
- Missing labels ⇒ report unlabeled coverage, don't guess.

## 11. Budget
Skeleton (host-only; F29 ships no firmware):
- New pytest tests: target ≥ 35; **0 removed or loosened** anywhere; full-repo `python -m pytest -q` must stay ≥ baseline (109 on an LF checkout — see status log for the CRLF worktree caveat).
- `python -m ruff check ml/eval` clean; stdlib only — **no new dependencies**, F00-owned `ml/pyproject.toml` untouched.
- Harness runtime: the 2-session synthetic set evaluates in < 2 s on the host.

## 12. Out of scope
- Model training.

## 13. Open questions / assumptions
- Labels come from user confirmations (F14), manual marks and synthetic scenarios; no external labeling protocol is in scope.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Skeleton runs on synthetic data in M2.

---

## 15. Implementation plan (M2 skeleton — PLAN stage, 2026-10-12)

**Report format design** (full contract in `docs/evaluation/REPORT_FORMAT.md`, JSON `K_REPORT_SCHEMA_VERSION`):
- Top-level keys: `schema_version`, `generated_by`, `disclaimer` ("possible/suspected events with confidence; not a diagnosis; not a medical device" — 00 §1), `config` (`event_match_tolerance_s`, `split: leave-one-session-out`), `dataset` (session ids, `total_duration_s`, `unlabeled_coverage`, `notes` carrying `raw_drops`/`line_drops`/`malformed_rows` so a degraded session is never presented as a clean result), `folds[]`, `aggregate`, `comparison[]` (one row per `detector_id` — heuristic vs model comparison lands in M5 on real data).
- Per fold and aggregate: `tp`, `fp`, `fn`, `precision`, `recall`, `f1`, `false_alerts_per_day`, `missed_event_rate`, latency `{mean_s, median_s, min_s, max_s}`, `false_positives[]` (session, `t_start_ms`, confidence, nearest-label dt), `false_negatives[]` (session, `t_start_ms`, nearest-prediction dt). No `accuracy` key in the skeleton (§5).
- Markdown sections, fixed order: Title + disclaimer · Configuration · Dataset (incl. data-quality notes) · Event-level metrics (aggregate table first, then per-fold) · **False positives** · **False negatives** (both always rendered, "none" when empty) · Detection latency · Unlabeled coverage · Model comparison.
- Harness CLI: `python ml/eval/run_eval.py --sessions <dir>... --out <dir> [--detector <id>] [--tolerance-s <float>]`; `--detector` defaults to the only detector found (error if several — one report per detector in the skeleton), `--tolerance-s` defaults to `K_EVENT_MATCH_TOLERANCE_S` and must be finite and > 0 (an infinite tolerance would match everything and inflate TP/recall); exit `K_EXIT_INPUT_ERROR` on missing/unreadable `session.json`, fewer than 2 sessions or duplicate session ids, `K_EXIT_SCHEMA_VERSION` on unknown `schema_version` (mirrors DATASET_FORMAT §4).

**M5 deferrals (marked, not implemented):** real trained models and heuristic-baseline comparison on real captures (blocked on F28 hardware DoD), leave-one-subject-out, resource-usage metrics (RAM/flash/CPU), plots, window-level metrics over full `features.csv` streams, optimal (bipartite) matching, end-to-end runs on real `tools/replay` outputs.

---
