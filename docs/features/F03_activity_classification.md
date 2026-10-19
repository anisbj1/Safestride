# F03 — Activity classification

> Track **A** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Classify broad activity context from `MotionFeatures`: Stationary/Sitting/Standing/Walking/Running/LyingResting/InactiveProlonged/Unknown, with confidence. Provides context for drowsiness inference. **Low movement never implies sleep.**

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `MotionFeatures`

**Outputs (produced contracts)**
- `ActivityEstimate` (1 Hz)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Infer sleep or drowsiness.
- Read raw IMU samples.
- Report Sitting/Standing/Lying with high confidence when orientation alone cannot support it.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Classifier type**
- Simplest: Threshold rules on ENMO, dominant frequency, tilt, stillness.
- Professional/scalable: Small trained tree/forest via F05 on a labeled activity dataset.
- Trade-offs: Rules are transparent and need no data; trained model generalizes better but needs labeled captures.
- **Choice:** Rules v1 (documented decision table). Interface allows a model later without changing consumers.

**D2. Posture from device orientation**
- Simplest: Report Sitting/Standing/LyingResting from tilt.
- Professional/scalable: Report `Stationary` and only emit posture with low confidence.
- Trade-offs: Arm position rarely equals body posture.
- **Choice:** Emit `Stationary` by default; posture values only with explicit low confidence, so downstream logic cannot over-trust them.

**D3. Walking/running cadence overlap (2.6–2.8 Hz)**
- Problem: `kWalkingCadenceMaxHz` (2.8) overlaps `kRunningCadenceMinHz` (2.6), so a window in the overlap matches both rows of the table.
- Simplest: give the whole overlap to one class (either 2.6–2.8 Hz is always Running, or always Walking).
- Professional/scalable: keep both bands and resolve the overlap with an independent cue (ENMO impact energy); a trained model (F05) later replaces the hand-tuned bands entirely.
- Trade-offs: the single-class rule is one line but systematically wrong for one population (fast walkers called runners, or runners called walkers); the energy gate costs one extra named constant but separates the classes by what actually differs (impact), and stays deterministic and testable.
- **Choice:** Running additionally requires `enmo_g ≥ kRunningEnmoMinG` (0.20 g). Priority when both match: Running > Walking. In the overlap with low energy the result stays Walking.

**D4. Flap control: min dwell vs. per-transition hysteresis**
- Problem: per-window rule output flickers at band/threshold boundaries; downstream (F07) must not see context churn.
- Simplest: emit the decision-table result raw every 1 Hz window.
- Professional/scalable: a full enter/exit hysteresis pair per class (8 transitions × 2 thresholds) — maximum control, but 16 thresholds to tune with no labeled data yet.
- Trade-offs: raw output flaps; full hysteresis is a large tuning surface. One shared min-dwell counter gives almost the same stability with a single knob.
- **Choice:** one dwell counter (`kMinDwellWindows` = 3 windows) gates **every** context change, *except* `quality == Insufficient`, which commits `Unknown` immediately (spec §10: stale context must not outlive the data that supported it). Cadence band edges get a small dedicated hysteresis (`kCadenceHysteresisHz`) inside `CadenceDetector` — in **both** directions: a widened exit band (class → `None`) and a class-to-class hold that skips the enter step while the current state's row still matches (§8 spells out both). A backwards or oversized `t_ms` jump resets the dwell — dwell counts *continuous* evidence.

**D5. Confidence model**
- Problem: `ActivityEstimate.confidence` must mean something consistent without over-claiming.
- Simplest: one fixed confidence per class.
- Professional/scalable: evidence-weighted confidence (share of satisfied gates) or an F05 model posterior behind the same field.
- Trade-offs: evidence-weighting manufactures precision the rules do not have; a model posterior needs labels that do not exist yet. Fixed values are transparent, testable, and leave the field free for a model later without touching consumers.
- **Choice:** `confidence = base[class] × quality_factor` (`kDegradedConfidenceFactor` = 0.75 for Degraded, 1.0 for Good), then the reporting floor (R8). Posture is capped at the floor (`kPostureConfidenceCap`), so Degraded data suppresses posture to `Unknown` first while robust classes stay reportable. `context == Unknown` always carries `confidence = 0.0` (no claim made).

**D6. Sitting/Standing in v1**
- Problem: the enum and spec list Sitting/Standing, but wrist tilt cannot separate them.
- Simplest: map tilt bands to Sitting/Standing at low confidence.
- Professional/scalable: per-user tilt calibration via F18 settings (the user's own "sitting arm" vs "standing arm" angles), or an F05 model over orientation + time + history.
- Trade-offs: tilt-band mapping fabricates a distinction the wrist does not support — sitting and standing have near-identical wrist-tilt distributions, so this would violate spec §5 ("not with high confidence when orientation alone cannot support it") in spirit; calibration/model need data that does not exist yet.
- **Choice:** v1 never emits Sitting/Standing. Still + upright/unknown orientation collapses to `Stationary`, explicitly permitted by `02 §4.3`. The enum values stay reserved (`ActivityContext` is append-only); `LyingResting` *is* emitted (still + near-horizontal tilt) but only at the capped low confidence.
- **[ASSUMED] tilt mounting convention (closed per DEC-019):** the 70–110° `LyingResting` band presumes how the device's z-axis sits relative to a resting forearm — no bench capture has confirmed it. Mounting/ergonomics are out of scope, so the band stays `[EXPERIMENTAL]` configuration in `activity_config.h` and is re-derived from a recording at F28/F29 if the mounting differs; `gravity_tilt_deg` itself is pinned by F02, so there is no contract impact.

### Decision table (feature DoD §14 — one rationale per row)

`still` ≔ `stillness_ms > 0` (F02's stillness state machine active) **or** `enmo_g < kStationaryEnmoMaxG`. The ENMO branch exists because F02 zeroes `stillness_ms`/`dominant_freq_hz` on non-Good windows; without it every Degraded window would collapse to Unknown and `kDegradedConfidenceFactor` would be dead. Evaluated top-down, first match wins; R2–R7 go through the D4 dwell, R1 commits immediately, R8/R9 transform the confidence afterwards.

| # | Condition | Output | Confidence | Rationale |
|---|---|---|---|---|
| R1 | `quality == Insufficient` | `Unknown` | 0.0 | No reliable data is a first-class output (00 §1; spec §10). Committed immediately so a stale label never outlives its evidence (D4). |
| R2 | `stillness_ms ≥ kProlongedInactivityMs` (10 min) | `InactiveProlonged` | 0.7 | Duration is the fact downstream needs; checked before posture because it dominates them. Explicitly *not* sleep — low movement never implies sleep (spec §1). |
| R3 | cadence = Running (2.6 Hz ≤ freq ≤ 4.0 Hz ∧ ENMO ≥ 0.20 g) | `Running` | 0.8 | Cadence band + impact energy are two independent cues agreeing — strongest row, hence the highest base confidence. Upper bound rejects vibration frequencies (§8). |
| R4 | cadence = Walking (1.2–2.8 Hz ∧ ENMO ≥ 0.03 g) | `Walking` | 0.75 | Step cadence (spec §8 derivation) plus whole-body motion; energy gate rejects tapping/fidget periodicity (D3). |
| R5 | `still` ∧ tilt ∈ [70°, 110°] | `LyingResting` | 0.5 (capped) | Still + near-horizontal is consistent with a resting posture; wrist alone cannot confirm lying vs. desk-rest, so it sits exactly at the reporting floor (D2) and any Degraded factor drops it to Unknown (R9). |
| R6 | `still` (otherwise) | `Stationary` | 0.7 | No meaningful motion; orientation cannot separate Sitting/Standing from this, so they collapse here (D6, `02 §4.3`). |
| R7 | moving, no cadence row matched | `Unknown` | 0.0 | Motion the table cannot name (slow gait below 1.2 Hz, chores, gestures): honest Unknown beats force-fitting a class (00 §7). |
| R8 | confidence < `kMinConfidenceToReport` (0.5) | `Unknown` | 0.0 | Reporting floor (spec §8): below it we say "no reliable classification" (applied after R9, so the floor sees the degraded confidence). |
| R9 | `quality == Degraded` | (post-step) | × `kDegradedConfidenceFactor` | A quarter off every confidence: posture (at the floor) drops to Unknown first, robust rows (0.7 → 0.525, 0.8 → 0.6) remain reportable. |

## 7. Module structure
- `bw/activity/activity_config.h` — `ActivityConfig` (thresholds, named + tagged constants)
- `bw/activity/cadence_detector.h` + `src/cadence_detector.cpp` — `CadenceDetector` (`step(freq_hz, enmo_g) → CadenceState`, band hysteresis)
- `bw/activity/activity_classifier.h` + `src/activity_classifier.cpp` — `ActivityClassifier` (`step(MotionFeatures) → ActivityEstimate`)
- `firmware/src/app/wiring/wire_activity.{h,cpp}` — construction only; the MotionFeatures→ActivityEstimate channel defers until a mailbox exists in `AppContext` (same deferral pattern as F01's SensorTask)

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kWalkingCadenceMinHz / MaxHz` | 1.2 / 2.8 | `DERIVED` | Typical human step cadence 72–168 steps/min |
| `kRunningCadenceMinHz` | 2.6 | `DERIVED` | Running cadence ≳ 156 steps/min |
| `kRunningCadenceMaxHz` | 4.0 | `DERIVED` | 240 steps/min — above any sustained human cadence; higher frequencies are vibration/noise, not running |
| `kCadenceHysteresisHz` | 0.2 | `EXPERIMENTAL` | Two mechanisms: (a) exit-band widening (≈12 steps/min) so a frequency hovering at a band edge cannot toggle `CadenceState` to `None` every window, and (b) the class-to-class *hold* — while the current state's own row still matches, the enter step is skipped, so the 2.6–2.8 Hz overlap is a Schmitt band (`Walking`→`Running` only above `kWalkingCadenceMaxHz`, `Running`→`Walking` only below `kRunningCadenceMinHz`). The class-to-class width is the band-edge gap `kWalkingCadenceMaxHz − kRunningCadenceMinHz` (= 0.2 Hz today, i.e. the same number but not read from the constant). ≈ F02's autocorrelation jitter, tune with data (D4) |
| `kWalkingEnmoMinG` | 0.03 | `EXPERIMENTAL` | Step cadence without whole-body motion is tapping/fidget, not walking (D3) |
| `kRunningEnmoMinG` | 0.20 | `EXPERIMENTAL` | Resolves the 2.6–2.8 Hz overlap: running impact is several × walking (D3) |
| `kStationaryEnmoMaxG` | 0.02 | `EXPERIMENTAL` | "No meaningful motion" for the `still` gate; deliberately at F02's stillness enter level so both stages flip at the same motion (redefined here: F03 may only depend on `bw_core`) |
| `kProlongedInactivityMs` | 600000 (10 min) | `EXPERIMENTAL` | Long enough to exclude pauses in conversation; tune with data |
| `kLyingTiltMinDeg / MaxDeg` | 70 / 110 | `EXPERIMENTAL` | ±20° around horizontal for the LyingResting row; wrist orientation noise while still |
| `kMinConfidenceToReport` | 0.5 | `EXPERIMENTAL` | Below this report `Unknown` |
| `kPostureConfidenceCap` | 0.5 | `EXPERIMENTAL` | Cap = floor, so posture is the weakest *reportable* claim and one Degraded factor (R9) suppresses it (D2/D5) |
| `kDegradedConfidenceFactor` | 0.75 | `EXPERIMENTAL` | A quarter off under Degraded IMU data; keeps robust rows ≥ floor (0.7 → 0.525) while posture falls out (0.5 → 0.375) |
| `kConfStationary / Walking / Running / InactiveProlonged` | 0.7 / 0.75 / 0.8 / 0.7 | `EXPERIMENTAL` | Ordered by cue strength: strongest = two agreeing cues (cadence + energy), weakest = single cue (stillness alone); placeholders for an F05 posterior later (D5) |
| `kMinDwellWindows` | 3 | `EXPERIMENTAL` | Rejects 1–2 window glitches; a genuine transition still appears within 3 s at 1 Hz (D4) |
| `kMaxFeatureStepGapMs` | 1500 | `DERIVED` | Expected 1 s window + 50 % scheduling slack; a larger jump means windows were lost, so dwell evidence is not continuous → reset dwell (D4) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Table-driven cases: synthetic feature sequences → expected context.
- Boundary/hysteresis around cadence and stillness thresholds.
- Unknown on Insufficient quality; no flapping (min dwell time).

## 10. Failure behavior
- Insufficient IMU quality ⇒ `Unknown`.
- Never crash on missing/invalid input; return `Err` or an output flagged `Insufficient`.

## 11. Budget
RAM < 1 KB; CPU negligible at 1 Hz.

## 12. Out of scope
- Fine-grained activities (cycling, stairs).
- Sleep staging.

## 13. Open questions / assumptions
- Whether to record labeled activity data early (F28) to train a small model.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Decision table documented in the spec with a rationale per row.

---
