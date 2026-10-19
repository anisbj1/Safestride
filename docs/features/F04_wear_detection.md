# F04 — Wear detection

> Track **A** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Decide whether the device is being worn (Worn / NotWorn / Uncertain) using PPG contact indicators and IMU micro-motion, so a stationary device on a desk is never interpreted as a sleeping user.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `PpgFeatures` (ir_dc_level, perfusion_index, hr_valid, sqi)
- `MotionFeatures` (micro-motion, stillness; tilt reserved — not scored in v1, see D4)

**Outputs (produced contracts)**
- `WearEstimate` (1 Hz)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Read raw sensors.
- Decide alertness or alerts.
- Return `Worn` without positive evidence.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Evidence combination**
- Simplest: Single rule: IR DC level above a threshold ⇒ worn.
- Professional/scalable: Weighted evidence score (PPG contact, pulsatility, IMU micro-motion, orientation) with hysteresis and minimum dwell.
- Trade-offs: The single rule breaks when contact quality or ambient light varies; the scored version needs tuning.
- **Choice:** Evidence score with a hysteresis band between two thresholds; inside the band the CURRENT state is held — from the default `Uncertain` that holds `Uncertain`, an in-band `Worn` stays `Worn` — so the band never forces `Uncertain`. Dwell times prevent flapping. Weights documented, tuned on synthetic and recorded fixtures (attached, resting, moving, removed).

**D2. Bias**
- Simplest: Default to Worn.
- Professional/scalable: Default to Uncertain until evidence accumulates.
- Trade-offs: Uncertain delays monitoring slightly after the sensor is attached.
- **Choice:** Default `Uncertain`; downstream logic treats Uncertain as 'no physiological inference'.

**D3. When is a source usable (quality gating)**
- Simplest: only `quality == Good` counts; anything else is treated as missing.
- Professional/scalable: a per-source trust weight derived from `DataQuality` (Good 1.0, Degraded 0.75, Insufficient 0) multiplied into the score.
- Trade-offs: the Good-only rule is easy to reason about but discards exactly the evidence that matters — F02 marks PPG `Insufficient` for low perfusion, which is the strongest not-worn reading; the trust-weight version is smooth but adds a tuning axis before any data exists.
- **Choice:** availability = "the window produced computed values", not "quality is Good". PPG counts when fresh with a computed DC (`ir_dc_level > 0`; F02 zero-fills an empty window), IMU counts when fresh and not `Insufficient` (below that F02's window statistics are undefined). Quality still shapes every score through `sqi` and `hr_valid`, so Degraded/Insufficient lowers the result instead of erasing it. This also refines §10: "both sources Insufficient ⇒ Uncertain" fires when neither source has usable values (stall/dropout), which is what §9's missing-PPG fallback means.

**D4. Score composition**
- Simplest: one rule per source (IR DC above threshold ⇒ worn; ENMO above floor ⇒ worn).
- Professional/scalable: weighted sub-scores per source (PPG: contact, perfusion, HR, SQI; IMU: micro-motion, stillness) blended by source weights; a learned combiner (F05/F29) can replace the weights later behind the same `WearEvidence` shape.
- Trade-offs: single rules are transparent but each breaks alone (ambient light lifts DC; a resting user sits under any single ENMO floor); weighted sub-scores need a documented weight table but degrade gracefully — no single cue dominates.
- **Choice:** weighted sub-scores, constants in `wear_config.h`, each group sums to 1 so the blend is a plain weighted mean. Contact carries the largest PPG share (only direct non-wear signal); micro-motion carries the largest IMU share (a desk device shows near-zero variance).

**D5. Meaning of `confidence`**
- Simplest: confidence = raw evidence score, regardless of reported state.
- Professional/scalable: confidence = evidence support *for the reported state* (score for Worn, 1−score for NotWorn, 0 for Uncertain), capped while a source is missing (chosen form caps only while contact evidence is missing — see below).
- Trade-offs: the raw score makes a confident-looking NotWorn-from-no-evidence possible; the support reading costs one branch and gives downstream one uniform meaning to trust.
- **Choice:** support-for-state; `Uncertain` reports 0 (we claim nothing); runs with no PPG contact evidence (IMU-only) are capped at `kImuOnlyMaxConfidence` so motion alone never produces a high-confidence claim (§9). PPG-only runs are not capped — contact is direct evidence and already scores on its own.

**D6. Time handling (staleness and discontinuities)**
- Simplest: trust the caller; use feature `t_ms` as-is.
- Professional/scalable: `step()` takes the tick time; features older than `kFeatureStaleMs` (or stamped in the future) count as absent; a backwards jump restarts any pending dwell.
- Trade-offs: trusting the caller is fewer bytes, but a stalled F02 task would keep feeding old contact as current evidence, and an unsigned underflow after a reboot could commit a state instantly; the guards cost ~20 bytes of state and two comparisons.
- **Choice:** the guards (host-tested): stall ⇒ sources absent ⇒ §10 Uncertain; backwards time ⇒ dwell restarts, never an instant commit.

## 7. Module structure
- `WearDetector`
- `WearEvidence` (per-signal scoring)
- `WearConfig`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kWornScoreEnter / kWornScoreExit` | 0.7 / 0.4 | `EXPERIMENTAL` | Hysteresis gap avoids flapping |
| `kNotWornConfirmMs` | 10000 | `EXPERIMENTAL` | Brief PPG dropouts during motion must not read as removal |
| `kWornConfirmMs` | 5000 | `EXPERIMENTAL` | Requires a few seconds of consistent evidence |
| `kDeskMotionEnmoMaxG` | 0.005 | `EXPERIMENTAL` | A device lying on a table shows near-zero variance; a held or worn device always has micro-motion |
| `kMicroMotionEnmoFullG` | 0.03 | `EXPERIMENTAL` | A resting-worn limb's micro-motion clears the desk floor by this much; slightly above F02's 0.02 g stillness gate so the score ramps there instead of pinning to 0 |
| `kPpgContactIrDcMin / kPpgContactIrDcFull` | 26214 / 65536 | `EXPERIMENTAL` | 10 %/25 % of the MAX30102 18-bit IR full scale (262143, F01): below = ambient leak, above = solid contact; LED current is F01's, tune with F29 |
| `kPpgPerfusionMin / kPpgPerfusionFull` | 0.001 / 0.005 | `EXPERIMENTAL` | 0.1 % AC/DC is this sensor's noise floor (F02 trusts beats there); 0.5 % is a firm pulse |
| `kDeskStillnessMs` | 300000 | `EXPERIMENTAL` | 5 min of continuous F02 stillness accumulates only for a device left untouched `[ASSUMED]` |
| `kFeatureStaleMs` | 3000 | `DERIVED` | 3 missed 1 Hz windows (02 §4.2): the source stalled, values describe the past |
| `kImuOnlyMaxConfidence` | 0.6 | `EXPERIMENTAL` | IMU alone cannot separate worn from held/carried; caps IMU-only runs below any "high confidence" claim (§9) |
| `kWearPpgWeight / kWearImuWeight` | 0.6 / 0.4 | `EXPERIMENTAL` | Contact is direct evidence, motion circumstantial; sums to 1 (no renormalization) |
| `kPpgContact/Perfusion/Hr/SqiWeight` | 0.40 / 0.25 / 0.20 / 0.15 | `EXPERIMENTAL` | Contact is the only direct non-wear cue; HR is derivative of perfusion and motion-gated; SQI partly double-counts perfusion; sums to 1 |
| `kImuMicroMotionWeight / kImuStillnessWeight` | 0.70 / 0.30 | `EXPERIMENTAL` | Micro-motion is primary; long stillness is anti-evidence only (a sleeping user can be still); sums to 1 |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Scenario fixtures: attached-at-rest, moving, resting-on-surface, removed, loose-contact, in-pocket.
- Hysteresis and dwell timing tests.
- Missing PPG → falls back to IMU-only with reduced confidence, never `Worn` with high confidence.
- Accepted residual (D5 cap, exercised by the `in-pocket` test): a carried/pocketed device with no PPG contact but sustained micro-motion may read as `Worn` at ≤ `kImuOnlyMaxConfidence` (0.6) confidence — downstream consumers treat ≤ 0.6 as weak wear evidence.

## 10. Failure behavior
- Both sources Insufficient ⇒ `Uncertain` (read as D3: neither source has usable values — no computed DC and no defined IMU window; immediate, no dwell).

## 11. Budget
RAM < 0.5 KB; negligible CPU.

## 12. Out of scope
- Extra hardware (capacitive sensing).

## 13. Open questions / assumptions
- Thresholds are `[EXPERIMENTAL]` defaults; tuning against recordings is optional.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Scenario-fixture results (pass/fail table) in the status log.

---
