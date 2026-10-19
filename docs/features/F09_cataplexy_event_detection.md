# F09 — Suspected cataplexy / event detection (experimental)

> Track **B** · Milestone **M5** · Runs on **firmware + ml** · Contract changes only via change request.

## 1. Responsibility
An **experimental** candidate detector plus a data-capture tool for cataplexy-like events. Phase 1 captures IMU/PPG windows around user-marked events to build a dataset; Phase 2 adds a rule-based candidate trigger; Phase 3 evaluates an ML classifier. Every output is a *suspected* event with confidence and reason codes — never a medical claim.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `MotionFeatures`, `PpgFeatures`, `StateSnapshot`
- `Command{MarkEvent}` (via `ICommandSink` consumer hook provided by wiring)
- `IModel` (F05, phase 3)
- Raw-window request through the event-window API (F18 via `IEventSink` payload flag)

**Outputs (produced contracts)**
- `Event{SuspectedCataplexy}` (confidence, reasons, context)
- Request to persist a raw sensor window around the event

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Claim diagnosis in any text/label.
- Alert directly (F10 decides).
- Capture raw windows outside Research mode (F24).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Delivery order**
- Simplest: Rule-based trigger only.
- Professional/scalable: Trained classifier with per-user personalization.
- Trade-offs: No labeled data exists; false alerts from falls/gestures likely.
- **Choice:** Capture-first: manual marking + window capture (Phase 1), then simple rules (Phase 2), classifier only after ≥ N confirmed events exist (Phase 3, N set by F29 analysis). Phase 3 is optional: if such data never exists, the interface plus tests on synthetic data are enough.

**D2. Confounders (falls, gestures, sudden stops)**
- Simplest: Ignore.
- Professional/scalable: Explicit exclusion features and low-confidence output when confounders present.
- Trade-offs: Reduces recall but keeps false alarms manageable.
- **Choice:** Log confounder flags in reason codes; evaluate false-alert rate per day (F29).
  *Note (2026-10-03): `ReasonCode` has no confounder values — see D4 and CR-009; until then the
  flags ride `ConfounderMask` + `Event::payload[1]`.*

**D3. How the raw-window request reaches F18 (§3 "event-window API")**
- Simplest: A flag bit in `Event::payload[0]` of the event F09 publishes; F18 dumps its ring
  buffer when it stores that event. No contract change, nothing to coordinate.
- Professional/scalable: A typed `IWindowSink { Result<void> request(CaptureSpan) }` in `bw_core`,
  with the dump, back-pressure and mode gating living in F18 and shared by F27/F28.
- Trade-offs: The payload flag costs nothing but is untyped, has 8 payload bytes total, and
  needs F18 to know F09's byte layout. The typed interface is self-documenting and reusable, but
  it is a contract change while neither F17 nor F18 exists yet — a proposal written against code
  nobody has written usually gets it wrong.
- **Choice:** Payload flag (bit0) now; `EventWindowRequester` still computes the span as an F09
  value type (`CaptureWindow`), so swapping in a typed sink later touches wiring only.
  `[ASSUMED]` F18 honours payload bit0 = "persist the window around this event" — verify against
  F18's spec when it lands; raise the typed-interface CR only if F18's design needs it.
  F18 must only dump bit0-flagged capture windows for events when `OperatingMode` is Research
  (F18 handshake item); F09 sets the flag as a request, never as a decision to capture (§5).

**D5. Confidence model for the rule trigger**
- Simplest: A boolean rule — "drop seen ⇒ event" — one threshold, no score.
- Professional/scalable: A weighted multi-feature score that later fuses `IModel::predict`
  (Phase 3) behind the same output struct.
- Trade-offs: Boolean has no gradation: a fall, a gesture and a clean drop all produce the same
  alert, so there is nothing for F29 to tune and no way to honour §8's "below threshold ⇒
  research stream only". A score costs more named constants and more tests, but gives exactly
  that knob and matches the project's "probabilities + deterministic rules" (00 §5).
- **Choice:** One calibrated score in [0,1]: `base × degraded-quality × HR-corroboration ×
  confounder-penalties`, clamped to [0,1]; `raised = confidence >= kMinCandidateConfidence`.
  Below the threshold the candidate is still returned (research stream), never published.
  Phase 3 replaces the *scoring* with `IModel` without changing `CandidateOutput`.

**D6. Evaluation latency: raise on the drop window or wait for a confirmation dwell**
- Simplest: Raise in the window where the drop is observed — no pending state, no extra latency.
- Professional/scalable: Hold the candidate for a dwell and require the stillness to persist
  (motion must not recover), which rejects brief gestures structurally.
- Trade-offs: The dwell needs a pending slot, more state and more tests, and delays a path that
  already waits `kPostEventCaptureMs` for its window; the gesture confounder is instead caught
  *without waiting* by looking one window further back (ShortMotionBurst).
- **Choice:** Immediate. D1 is capture-first and alerts are F10's decision anyway (§5), so
  latency buys nothing today; F29's false-alert-rate analysis can re-open a dwell if the data
  asks for it.

**D7. Which evidence the detector accepts at all**
- Simplest: Run the rule on whatever motion windows arrive.
- Professional/scalable: Gate on source quality and wear, each gate producing its own reason
  code, and drop the history while the device is not worn.
- Trade-offs: Gating can lose candidates while F07 is still booting, but the alternative
  "detects" a bracelet left on a table — a fabricated confidence value, which 00 §1 forbids.
- **Choice:** Gate. No `StateSnapshot` ⇒ nothing evaluated (confidence 0, no reasons — "no
  reliable data" is a first-class output); `wear != Worn` ⇒ `DeviceNotWorn` + history reset;
  `Insufficient` ⇒ `LowSignalQuality`; `Degraded` ⇒ keep, with a penalty and `LowSignalQuality`;
  absent IMU ⇒ `disabled` + `SensorUnavailable` (§10).

## 7. Module structure
- `EventCandidateDetector`
- `ConfounderChecker`
- `EventWindowRequester`
- `ml/events/{label_tools.py,train.py}`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kAbruptDropWindowMs` | 2000 | `EXPERIMENTAL` | Window in which a sudden loss of arm motion is evaluated |
| `kPreEventCaptureMs` | 30000 | `EXPERIMENTAL` | Context before the marker |
| `kPostEventCaptureMs` | 30000 | `EXPERIMENTAL` | Context after the marker |
| `kMinCandidateConfidence` | 0.4 | `EXPERIMENTAL` | Below this, do not raise an event; log to research stream only |
| `kFeatureWindowMs` | 1000 | `DERIVED` | F02 emits `MotionFeatures`/`PpgFeatures` per 1 s window (02 §4.2) |
| `kDropHistoryWindows` | 3 | `DERIVED` | `ceil(kAbruptDropWindowMs / kFeatureWindowMs)` + 1: the drop window plus one earlier window to spot a short burst |
| `kHistoryMaxGapMs` | 2000 | `DERIVED` | = `kAbruptDropWindowMs`: one missed window still proves the claim, two do not ⇒ history restarts (also on a backwards timestamp) |
| `kActiveMotionEnmoG` | 0.10 | `EXPERIMENTAL` | ENMO at/above which the arm was demonstrably moving before the drop (between resting ~0.03 g and walking) |
| `kPostDropEnmoG` | 0.03 | `EXPERIMENTAL` | ENMO at/below which the arm is "still" — the worn resting-limb band, not the "device on a table" band |
| `kFallJerkRmsGPerS` | 2.0 | `EXPERIMENTAL` | Jerk RMS of an uncontrolled descent (fall confounder) |
| `kFallPeakEnmoG` | 0.8 | `EXPERIMENTAL` | Impact-sized excess over 1 g in the active window (fall confounder) |
| `kPostureTiltDeltaDeg` | 30 | `EXPERIMENTAL` | Gravity-angle change across the drop that means the wrist re-oriented (sitting down) |
| `kMinHrConfidence` | 0.5 | `EXPERIMENTAL` | PPG beat confidence below which `hr_bpm` is treated as absent |
| `kHrDropBpm` | 8 | `EXPERIMENTAL` | Pre-to-post HR drop that counts as PPG corroboration |
| `kBaseCandidateConfidence` | 0.75 | `EXPERIMENTAL` | Score of a clean drop with no confounders |
| `kFallPenaltyFactor` | 0.4 | `EXPERIMENTAL` | ×0.4 ⇒ a fall lands below the raise threshold |
| `kPostureChangePenaltyFactor` | 0.5 | `EXPERIMENTAL` | ×0.5 ⇒ a posture change lands below the raise threshold |
| `kBriefBurstPenaltyFactor` | 0.5 | `EXPERIMENTAL` | ×0.5 ⇒ a gesture lands below the raise threshold |
| `kDegradedQualityPenaltyFactor` | 0.8 | `EXPERIMENTAL` | `Degraded` windows still count, but weaker (00 §1) |
| `kHrCorroborationFactor` | 1.15 | `EXPERIMENTAL` | Multimodal match is rewarded, never enough alone to override a confounder — the factor (and its `HrTrendChange` reason) is **skipped entirely whenever any confounder flag is set**, so the ×0.4/×0.5 penalties always land below the raise threshold on their own |
| `kMaxPendingCaptures` | 1 | `DERIVED` | One 60 s capture in flight: a second mark inside that span describes the same episode (§11 budget) |
| `kWindowRequestFlag` | `0x01` | `EXPERIMENTAL` | `payload[0]` bit0 = "persist the window around this event" (D3, `[ASSUMED]` F18 honours it) |
| `kCataplexyDetectorId` | 9 | `ASSUMED` | `Event::detector_id`; bw_core defines no registry yet — confirm with the integrator |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).
The same values live, tagged and with their full rationale, in
`firmware/lib/bw_cataplexy/include/bw/cataplexy/cataplexy_config.h`; tunable knobs are
collected in `CataplexyConfig` so tests and later `[USER]` settings share one path.

## 9. Test plan
- Synthetic 'abrupt drop' vs 'sitting down' vs 'fall' fixtures for the rule trigger.
- Window capture boundaries (pre/post) on the ring buffer.
- Confidence never reported without reason codes.

## 10. Failure behavior
- IMU failed ⇒ detector disabled and reports `SensorUnavailable`.

## 11. Budget
RAM: raw window buffer counted in F18 budget (≈ 60 s × 50 Hz × 12 B ≈ 36 KB int16-packed) — confirm with F18; disable in LowPower profile if needed.

## 12. Out of scope
- Any clinical validation.
- Video-based detection (F27 remains separate).

## 13. Open questions / assumptions
- Which manual-mark gesture (F11).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Spec states clearly which phase was implemented and what is still hypothesis.

### Phase status (DoD §14)

| Phase / part | Content | Status |
|---|---|---|
| 1 | manual mark → window span (`EventWindowRequester`) | **Pure logic implemented + host-tested.** Deferred: the 1 Hz/mark pump and the F18 hand-off (neither F07's channel nor F18 exists yet) |
| 2 | rule-based candidate trigger (`EventCandidateDetector` + `ConfounderChecker`) | **Implemented + host-tested** (synthetic fixtures per §9). Hypothesis: all thresholds are `[EXPERIMENTAL]` until F28/F29 recordings |
| 2b | publish `Event{SuspectedCataplexy}` | **Implemented as a pure builder** (`makeSuspectedCataplexyEvent`); publishing through F17's `IEventSink` waits for the pump |
| 3 | ML classifier (`IModel`) | **Not started.** Hypothesis only: needs ≥ N confirmed events (D1, N from F29) |
| — | raw ring buffer in RAM (§11) | **Not started** — belongs to F18; F09 only computes the span |
| — | confounder flags in `ReasonCode` (D2/D4) | **Pending CR-009** (proposed 2026-10-03); payload adapter meanwhile |
| — | `ml/events/{label_tools.py,train.py}` | **Not started** — Phase 3 tooling |

---
