# F06 — Wakefulness / drowsiness estimation

> Track **B** · Milestone **M2 (heuristic) → M5 (trained)** · Runs on **firmware + ml** · Contract changes only via change request.

## 1. Responsibility
Assemble the multimodal feature vector (motion, PPG, activity, time-of-day, recent history, baseline-relative values) and produce `AlertnessProbabilities`. First a transparent heuristic scorer; later a trained model via F05. Output goes to the State Engine — never straight to alerts.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `MotionFeatures`, `PpgFeatures`, `ActivityEstimate`, `SourceQuality`
- `IBaselineProvider` (optional; absence must work)
- `IClock` (time-of-day; may be unknown)
- `IModel` (F05)

**Outputs (produced contracts)**
- `AlertnessProbabilities` (1 Hz or per epoch)
- Contribution data for F22 (top factors as `ReasonMask`)
- `ml/wakefulness/schemas/feature_schema_v1.json` (registered per the F05 process in `contracts/FEATURE_SCHEMA.md`)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Emit alerts or states.
- Fabricate probabilities when inputs are `Insufficient` (set `valid=false`).
- Depend on F13's classes.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Estimator v0**
- Simplest: Sum of hand weights → softmax.
- Professional/scalable: Trained model from day one.
- Trade-offs: No labeled data exists; a trained model with none would be fiction.
- **Choice:** Heuristic v0 with documented rules per term (M2); data collection through F28; trained linear/tree model behind `IModel` in M5.

**D2. Epoch vs instantaneous**
- Simplest: Score every second.
- Professional/scalable: Aggregate to 30 s epochs, keep recent epoch history features.
- Trade-offs: Epochs delay reaction by up to ~30 s but stabilize noisy features.
- **Choice:** Compute features per second, decide per epoch with a rolling window; smoothing/persistence remain in F07.

**D3. Where the `ReasonMask` lives before the contract carries one**
- Problem: §3 promises "contribution data as `ReasonMask` for F22", but `AlertnessProbabilities` (02 §4.4) has no such field and §4 forbids editing contracts directly.
- Simplest: Drop the mask at the seam — F06 emits probabilities only, F22 explains with F07's codes. Rejected: the evidence is computed then thrown away, and F07 observes only estimate structs, so nobody could ever set F06's bits.
- Alternatives: (a) keep a private adapter struct in F06; (b) raise a CR adding the field; (c) ship a parallel side-channel struct.
- Trade-offs: (c) costs a second timestamp to keep consistent; (b) is blocked on review; (a) is invisible to others but costs nothing and unblocks all of F06.
- **Choice:** (a) now — `WakeEstimate { AlertnessProbabilities probs; ReasonMask reasons; }` inside `bw_wake` — plus (b) as **CR-010** (`docs/change_requests/CR-010_alertness_reasons.md`, additive: append `ReasonMask reasons` to `AlertnessProbabilities`; CR-007…009 were taken by parallel work the same day). On acceptance, `wire_wake.cpp` passes the field through in a two-line switchover logged below. Most scalable option would be a typed evidence block with per-feature magnitudes (see CR-010 alternatives) — deferred until F22 asks for more than bits.

**D4. What epoch history stores**
- Problem: §8 `kHistoryEpochs` = 10 epochs of context for `low_motion_epochs` / `enmo_drop_ratio`, in < 4 KB RAM.
- Simplest: Keep the last 10 raw feature windows (10 × full vectors). Rejected: ~16 floats × 10 epochs for data nobody scores again, and it grows with the schema.
- Most scalable: Ring buffer of full `FeatureVector`s plus derived history features — flexible, but re-derives what is already derived.
- **Choice:** Fixed per-epoch accumulators (already-computed sums/counts) + `RingBuffer<EpochSummary, kHistoryEpochs>`: one compact struct per epoch, O(1) push, history features computed once from summaries. Bounded, schema-independent, and it swaps for a larger/disk-backed store if F28 ever wants months of epochs.

**D5. How the three probabilities are formed (heuristic path)**
- Simplest: Normalize non-negative evidence scores to sum to 1 (weights directly as probabilities).
- Most scalable: Trained model behind `IModel` (F05) — but there are no labels yet (§13, D1), so shipping that now would be fiction.
- Trade-offs: Normalized-sum ignores class geometry (a single strong score can swallow the other two) and cannot express "all three mediocre"; softmax over logits gives a proper distribution, is monotone in each logit, and its logits stay hand-weighted until training data exists.
- **Choice:** Max-shifted 3-class softmax over hand-weighted logits (`[EXPERIMENTAL]`, documented per term), producing `awake/drowsy/sleep`, confidence = max p. The `IModel` path in M5 replaces only the estimator, not the assembler or the output struct.

**D6. Schema header: mirror vs generated**
- Problem: `FeatureVector` positions and normalization constants must be identical in `ml/wakefulness/schemas/feature_schema_v1.json` and firmware; the generated header (FEATURE_SCHEMA §2.2) lives in `firmware/lib/bw_ml/` which F06 may not include (`01` §1 rule 1) and which does not exist yet.
- Simplest: Let the assembler read the JSON — impossible on-device (no filesystem in the estimator's world).
- Most scalable: Root-injected generated header (F05 D4) — one source of truth, no hand sync.
- Trade-offs: The scalable path is blocked until M5, so until then someone must keep C++ constants in step with the JSON.
- **Choice:** Hand-maintained mirror `bw/wake/alertness_schema.h` (constants + `static_assert` well-formedness), guarded by `ml/wakefulness/test_schema_parity.py` so JSON↔header drift fails pytest. `[ASSUMED]` schema path: the spec §3 places the JSON at `ml/wakefulness/schemas/`, FEATURE_SCHEMA §1 says `ml/common/schemas/` — F06 keeps its copy where its own spec says and flags it as an open question for F05. M5 switches to the injected generated header; the mirror is deleted, not kept.

**D8. What the heuristic scores**
- Simplest: Score the normalized `FeatureVector` (what the model would see).
- Most scalable: Score through the schema regardless of estimator, so heuristic and model share one path.
- Trade-offs: Normalized units hide the anchors the rules were written in ("ENMO < 0.03 g", "HR 12 bpm below baseline") — a rule book readable only after dividing by σ; and normalizing twice (once for the heuristic, once for the model) invites drift.
- **Choice:** The heuristic scores the `EpochSummary`'s raw physical units — the documented rule book stays checkable by eye — while only the `IModel` path consumes the normalized vector (single normalizer in `FeatureAssembler`). The scalable convergence (one scored representation) happens at M5 when the heuristic retires; until then readability beats symmetry.

**D9. Missing input inside an epoch**
- Simplest: Skip the feature — impossible, `FeatureVector` is positional (FEATURE_SCHEMA §2.1: order is normative, count fixed).
- Next: Sentinel value (`-1`, NaN) — explicitly forbidden (`01`: no magic values, NaN-as-meaning).
- Most scalable: Impute from a running personal baseline — needs F13's baseline history, which may be absent, and invents data before it exists.
- **Choice:** A numeric feature with no data is written at its **normalization identity** (schema mean → normalized 0; `method: none` → 0) with a companion flag/ratio feature that says the value is absent (`hr_valid_ratio`, `baseline_valid`, `tod_valid`, …). Nothing is invented, the model can learn "flag low ⇒ ignore value", and F06's own validity gate (`valid=false` on `Insufficient`) still protects the output.

## 7. Module structure
### D12. Repair confirmed pipeline-age rejection without relaxing cadence/wear
- Evidence: authorized diagnostic capture reports `motion_age_ms=1992`,
  `cadence_ms=1000`, wear age 961–981 ms, Good IMU and `reject=motion_stale`
  throughout Worn intervals (including a Worn/Good IMU/Good PPG snapshot).
  Log: Temp/opencode/wakefulness2_gate_bench.log. This confirms the failing
  gate, but does not prove which individual added operation shifted task phase.
- Simplest: poll F06 faster. This reduces its own consumer delay but does not
  account for F02 publication lag or F07 forwarding to F10. Global widening of
  the reused 1500 ms constant would also incorrectly widen cadence/wear rules.
- Scalable: event-driven scheduling with explicit per-stage deadline contracts.
  Deferred: requires shared runtime changes beyond this targeted repair.
- Choice: derived, stage-specific age budgets with original source stamps.
  F06 motion age is 1000 ms F02 publish + 1000 ms F06 consume + 500 ms slack
  = 2500 ms. Independent wear age, accepted-response age and F06 inter-call
  gap each retain 1500 ms. F10 allows another 1000 ms F07 phase + 50 ms tick
  (3550 ms total including the same slack); F10 continuity remains 1500 ms.
  No motion, confidence or sleep threshold changes; no forced Awake.
- F10's new observation check uses its monitoring-session start, not the last
  countdown reset's receipt time. Otherwise subsequent delayed-but-new movement
  windows are skipped until their source stamps catch up to the last receipt.
  Per-source timestamp deduplication and pre-session rejection remain intact.

### D13. Raise the gentle-movement threshold to reject stillness noise (2026-10-19)
- Problem: after D12 was uploaded, the developer reports the 20 s timer remains
  reset and suspects small stillness/noise ENMO fluctuations are qualifying.
  The diagnostic bench capture had no sustained Worn interaction, so it cannot
  quantify the noise floor.
- Simplest: use the 0.03 g epoch anchor for both concerns. Rejected: it couples
  the demo response to heuristic epoch classification and may miss gentle input.
- Scalable: learn per-device thresholds from labeled still/moving traces.
  Deferred until F28/F29 data exists.
- Choice: user-directed `kDemoAwakeMotionEnmoG=0.04 g`; only ENMO strictly
  greater than 0.04 qualifies for demo Awake evidence and F10 countdown resets.
  Keep epoch low-motion at 0.03 g and strong burst at 0.35 g. This is an
  experimental bench threshold, not a measured separation of noise and movement.

### D11. Diagnose the post-tuning Unknown regression before changing age limits
- Problem: Worn/Good inputs reportedly became Unknown after commit `05c6007`.
  That commit added floating-point serial telemetry (subsequently repaired),
  timestamped movement evidence and its F10 consumption. The existing F06
  1500 ms motion/wear/cadence gates predate the regression; source receipt
  alignment alone does not establish which gate failed on the device.
- Simplest: widen all reused freshness limits or force Awake. Rejected:
  masks wear/cadence failures and would change safety behavior without proof.
- Scalable: a queued, timestamped pipeline trace across all tasks. Deferred:
  changes shared runtime plumbing for a targeted regression investigation.
- Choice: retain existing gates, report the first failing input gate from
  the same predicate used by `step()`, and append integer-only `WAKEGATE`
  diagnostics at consumption (sample time, motion/wear age and ordering,
  presence, cadence, qualities, output validity). Local diagnostic enum only;
  no shared contract changes. Keep existing WAKEMOTION text compatible.
- Source verification: F02 `ImuWindowProcessor::close` stamps **window end**,
  not start. The scheduler and independently phased 1 Hz F02/F06 consumers
  can still add publication/consumption latency. Synthetic 2033 ms motion
  age reproduces `MotionStale` with fresh Worn and Good quality; this is not
  yet proof of the actual device rejection. Any repair must retain the
  original evidence timestamp and separate age budgets from continuity gaps.

### D10. Hardware stack-canary failure after demo telemetry (2026-10-19)
- Problem: device serial log reports `Stack canary watchpoint triggered (F06Pump)`
  immediately after tasks release; the OLED briefly draws before the ESP32
  reboots. The new floating-point `Serial.printf` is the leading trigger on
  the existing 2048-byte task stack. Host tests cannot reproduce this ESP32
  task-stack failure.
- Simplest: increase stack only. Rejected as the sole fix: leaves costly
  newlib floating-point formatting in a small real-time estimation task.
- Scalable: queue telemetry to a dedicated low-priority logging service.
  Deferred: requires new cross-feature runtime plumbing for two measurements.
- Choice: integer-only micro-g formatting with finite/range validation and
  the existing serial text schema; reserve 4096 bytes for the F06 task. This
  removes the suspected heavy call path while allowing headroom for epoch
  estimation and diagnostics. The allocation remains a hypothesis until the
  corrected build's stack watermark and sustained uptime are checked on device.
  It changes neither the 0.015 g qualification nor any estimator thresholds.
- Startup logs also show storage/event recovery before task release (example:
  event recovery ready at 14555 ms). Do not call that an OLED timeout or erase
  stored events. Recheck cold-boot timing after the reboot loop is resolved;
  storage startup optimization is outside this targeted F06 repair.

- `FeatureAssembler` (schema-driven)
- `HeuristicAlertnessEstimator`
- `ModelAlertnessEstimator` (wraps `IModel`)
- `AlertnessEstimator` interface (internal)
- `ml/wakefulness/{train.py,features.py}`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kEpochLengthMs` | 30000 | `DERIVED` | Actigraphy convention; balances noise vs latency |
| `kWakePumpStackBytes` | 4096 | `EXPERIMENTAL` | 2 KB canary failure after telemetry; reserve headroom and measure on device |
| `kTelemetryMicroGPerG` | 1000000 | `DERIVED` | Micro-g integer logging avoids float printf while preserving gentle threshold precision |
| `kTelemetryRoundingMicroG` | 0.5 | `DERIVED` | Round the displayed measurement only; qualification uses original float |
| `kHistoryEpochs` | 10 | `EXPERIMENTAL` | 5 min of context; tune with data |
| `kMinInputQuality` | Degraded | `EXPERIMENTAL` | `Insufficient` inputs ⇒ `valid=false` |
| `kLowMotionEnmoG` | 0.03 | `EXPERIMENTAL` | Heuristic anchor for 'sustained low motion' |
| `kFeatureWindowMs` | 1000 | `EXPERIMENTAL` | F02 emits 1 s pairs; 30 windows per epoch |
| `kDemoMotionFreshnessMs` | 2500 | `DERIVED` | F02 publication + F06 consumption + 500 ms slack (D12) |
| `kDemoInputGapMs` | 1500 | `DERIVED` | Consecutive F06 cadence, not pipeline age |
| `kDemoWearFreshnessMs` | 1500 | `DERIVED` | F04 production-to-F06 consumption, no F02 delay |
| `kDemoResponseFreshnessMs` | 1500 | `DERIVED` | F10 acceptance-to-F06 consumption, no F02 delay |
| `kDemoAwakeMotionEnmoG` | 0.04 | `EXPERIMENTAL` | User-selected cutoff to reject observed stillness noise; strict `>` |
| `kMinSamplesPerEpoch` | 15 | `EXPERIMENTAL` | Epochs sparser than half the windows report no estimate (§5) |
| `kMinActivityConfidence` | 0.5 | `EXPERIMENTAL` | F03's own reporting floor, redefined (01 §1 rule 1) |
| `kHrDrowsyZFull` | -1.0 | `EXPERIMENTAL` | Full HR evidence at 1 MAD below personal baseline |
| `kMinHrMadFloorBpm` | 3.0 | `EXPERIMENTAL` | Noise floor when F13's MAD is unknown (no ÷0) |
| `kHrTrendDropBpm` | 5.0 | `EXPERIMENTAL` | ~7 % of resting HR: above beat noise, below posture change |
| `kSleepLowMotionEpochs` | 4 | `EXPERIMENTAL` | 2 min sustained low motion separates stillness from sleep |
| `kLowMotionEpochRatio` | 0.5 | `EXPERIMENTAL` | Majority of windows low-motion ⇒ epoch is low-motion |
| `kTodDrowsyStartMin`/`EndMin` | 840/960 | `EXPERIMENTAL` | Classic 14:00–16:00 post-lunch dip; F13 replaces it later |
| `kMinHrValidRatio` | 0.5 | `EXPERIMENTAL` | HR over <half the epoch is a guess, not a measurement |
| `kWakeBias` / `kDrowsyBias` / `kSleepBias` | 1.0 / 0.0 / -1.0 | `EXPERIMENTAL` | Softmax priors: awake ahead, drowsy needs evidence, sleep needs duration |
| `kWActiveMotion` / `kWLowMotion` | 1.2 / 1.2 | `EXPERIMENTAL` | Strongest cue and its exact complement (⇒ §9 monotonicity by construction) |
| `kWInactive` / `kWAbruptMotionDrop` | 0.6 / 0.6 | `EXPERIMENTAL` | Stationary meeting ≠ drowsiness; stopping ≠ microsleep |
| `kWHrBelowBaseline` | 0.8 | `EXPERIMENTAL` | Real but single-modality cue, below motion weights |
| `kWHrTrendDrop` | 0.4 | `EXPERIMENTAL` | Noisier difference of two 30 s means |
| `kWTimeOfDay` | 0.5 | `EXPERIMENTAL` | Context nudges, never decides |
| `kWSleepDuration` / `kWSleepStill` | 1.0 / 0.8 | `EXPERIMENTAL` | Duration separates sleep from 30 s of sitting; stillness alone is never sleep (F03) |
| `kBaseConfidence` | 0.8 | `EXPERIMENTAL` | Hand-weighted rules never claim certainty (§10) |
| `kConfidenceGoodFactor` | 1.0 | `DERIVED` | Good data earns the full ceiling — no penalty for a healthy pipeline (the complement of the degraded factor; never tuned) |
| `kConfidenceDegradedFactor` | 0.75 | `EXPERIMENTAL` | Degraded data costs 25 %, mirrors F03 |
| `kConfidencePpgMissingFactor` | 0.8 | `EXPERIMENTAL` | IMU-only epoch: cardiovascular cue absent (§10) |
| `kConfidenceMissingOptionalFactor` | 0.9 | `EXPERIMENTAL` | Each absent optional source (baseline/clock) costs 10 %; an absent history is *not* a factor — it reads as the streak/drop history features being 0 (D9) |
| `kReasonActivationThreshold` | 0.5 | `EXPERIMENTAL` | Half-strength terms are not quotable as reasons (F22) |
| `kMinContributionMagnitude` | 0.1 | `EXPERIMENTAL` | Below this, a "top factor" in 16 features is noise |
| unit conversions (`kMsPerSecond`, `kSecondsPerMinute`, `kSecondsPerDay`, `kMinutesPerDay`, `kTwoPi`) | 1000 / 60 / 86400 / 1440 / 6.2831853 | `DERIVED` | Arithmetic identities; never tuned |

Every constant lives in `firmware/lib/bw_wake/include/bw/wake/wake_config.h` with its full
rationale, is reachable at runtime through `WakeConfig` (tests/F18 can override without
code changes), and is `[EXPERIMENTAL]` unless it is an identity. All values are starting
points unless tagged `[HW]`; tune with data (F28/F29) and log tuned values back here.

## 9. Test plan
- Table tests for heuristic rules; monotonicity (less motion + lower HR never lowers drowsy probability).
- `valid=false` on insufficient quality; works with baseline absent.
- Schema check between assembler and model metadata.

## 10. Failure behavior
- PPG unavailable ⇒ IMU-only feature set with reduced confidence, flagged in output.
- Never crash on missing/invalid input; return `Err` or an output flagged `Insufficient`.

## 11. Budget
RAM < 4 KB; CPU < 1 ms per decision.

## 12. Out of scope
- State transitions, hysteresis (F07).
- Cataplexy features (F09).

## 13. Open questions / assumptions
- Labels come from user confirmations (F14), manual marks and synthetic scenarios; no external labeling protocol is in scope.
- **`[ASSUMED]` schema path:** §3 puts the JSON at `ml/wakefulness/schemas/`, FEATURE_SCHEMA §1 says `ml/common/schemas/` — F06 follows its own spec; F05 to confirm (status log 2026-10-03).
- **`[ASSUMED]` schema id:** FEATURE_SCHEMA §2.4 reserves id 1 for this feature set "confirmed when F06 defines its schema" — F06 confirms id 1, version 1 (status log 2026-10-03).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Every heuristic term has a documented rationale and reason code.
- **CR-010** (`AlertnessProbabilities` + `ReasonMask reasons`) accepted, or the status log records that F06 shipped with the local adapter.

---
