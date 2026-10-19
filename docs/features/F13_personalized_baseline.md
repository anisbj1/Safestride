# F13 — Personalized baseline

> Track **B** · Milestone **M5** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Learn the user's normal patterns (resting HR, typical activity, inactivity patterns, normal sensor ranges) and expose them as a `BaselineProfile`. Separated from inference: consumers must work when the baseline is absent.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `PpgFeatures`, `MotionFeatures`, `ActivityEstimate`, `WearEstimate`, `SourceQuality`
- `IKeyValueStore` (persist profile)
- `OperatingMode` (Calibration)

**Outputs (produced contracts)**
- `IBaselineProvider` / `BaselineProfile`
- Baseline-updated event (optional)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Update from bad-quality data.
- Change model behavior directly (only exposes a profile).
- Learn during suspected events (poisoning).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Estimator** *(refines the original D1: "Robust EWMA/median, single bucket v1")*
- **Problem:** turn sparse, artifact-prone qualified samples into `resting_hr_bpm` + `resting_hr_mad` + `typical_enmo_g`, robust to outliers and to slow drift, with persisted state that fits the §11 flash budget (< 200 B).
- **Simplest:** running mean of HR (count + sum, 8 B state). No robustness at all: one 150 bpm artifact poisons the mean for hours.
- **Alternatives:** (a) median/MAD over a fixed ring of the last N qualified samples — fully robust and exact, but a 64-sample ring of paired (hr, enmo) floats is 512 B of state to persist, breaking §11; (b) EWMA of the value + EWMA of the absolute deviation (a MAD proxy) with incremental-mean seeding and an outlier gate — 3 floats of persisted state.
- **Trade-offs:** the ring resists every outlier; the EWMA is vulnerable during the short seeding ramp (mitigated by the hard wear/quality/activity/plausibility gates) but is ~16× smaller to persist, O(1) per sample, and warm-restarts from a summary instead of relearning.
- **Choice:** (b). Update rule (pure, per accepted sample x, n = accepted count before update):
  `alpha = (n < kEwmaRampSamples) ? 1/(n+1) : 1/kEwmaSteadyWindowSamples`;
  reject when `n >= kEwmaRampSamples` and `|x − hr_ewma| > max(kMinOutlierBandBpm, kOutlierBandSigma · hr_absdev_ewma)`;
  else `dev = |x − hr_ewma|` (pre-update), `hr_ewma += alpha·(x − hr_ewma)`, `hr_absdev_ewma += alpha·(dev − hr_absdev_ewma)`; `enmo_ewma` uses the same alpha on accepted samples, without a separate ENMO deviation/outlier gate. `resting_hr_mad = hr_absdev_ewma` — F06 already floors it (`feature_assembler.cpp:282`, `min_hr_mad_floor_bpm`), so a near-zero early MAD cannot produce a fabricated z-score. `kEwmaSteadyWindowSamples` derives from `kAdaptationTimeConstantH` (see §8). No sqi gate: F02 already folds sqi/motion into `PpgFeatures::quality`, a second threshold would be gate creep. Single bucket in v1; time-of-day buckets are a later, additive change.

**D2. Calibration and mode absence** *(refines the original D2: "both")*
- **Problem:** seed fast when the user cooperates, adapt slowly afterwards — and stay correct when `OperatingMode` is absent, because F24 lands in the same wave.
- **Simplest:** always-on learning, one fixed gate, ignore mode entirely.
- **Professional/scalable:** explicit Calibration mode plus continual slow adaptation.
- **Trade-offs:** mode-gated math creates two behaviors to test and must default correctly when the mode channel is unknown.
- **Choice:** one learning path, always on (seeding = the EWMA ramp, adaptation = the steady EWMA); `OperatingMode::Calibration` **only widens the activity gate** to {Stationary, Sitting, Standing, LyingResting} (wear + quality gates stay hard) so a misclassified still posture during a guided calibration still counts. Mode absent/unknown ⇒ wiring passes `calibration=false` ⇒ normal gate (Day-equivalent). No other math depends on mode.

**D3. Persistence format** *(new decision)*
- **Problem:** survive reboot without relearning 30 min, detect corruption (§10), fit §11.
- **Simplest:** persist the `BaselineProfile` (20 B) raw. Loses ramp state (alpha restarts ⇒ slow re-converge after every reboot) and cannot detect corruption.
- **Professional:** versioned blob with magic, schema version, flags, payload, CRC-32.
- **Choice:** 32-byte blob under key `"f13.baseline"` via `IKeyValueStore` (`f18::kvStore()`): `u32 kBlobMagic` ('F13B'), `u16 kBlobSchemaV1`, `u16 flags` (reserved 0), `f32 hr_ewma`, `f32 hr_absdev_ewma`, `f32 enmo_ewma`, `u32 accepted_samples`, `u32 reserved` (0), `u32 crc32` (poly 0xEDB88320). CRC-32: `bw_storage`'s copy (`bw/storage/record_codec.h`) is not includable from a feature lib (01 §1 rule 1), so a ~15-line bitwise implementation lives in `baseline_serializer.cpp` — same polynomial, values comparable, no contract change. Unknown schema ⇒ `Err::Unsupported` ⇒ same discard path as corrupt (future versions bump the schema; decoding v1 is optional — prototype-acceptable).

**D4. Publish seam / cross-task reads** *(new decision)*
- **Problem:** F06's pump reads the profile from another task; `BaselineProfile` is a 20 B multi-word struct — torn reads possible.
- **Simplest:** hand F06 the `BaselineTracker` itself (racy). **Professional:** double buffer/seqlock inside the tracker — but locking inside pure L2 logic adds platform calls (forbidden).
- **Choice:** pure `BaselineTracker` (not thread-safe, single writer = the F13 pump) + wiring-level `BaselineProviderAdapter : bw::IBaselineProvider` in `wire_baseline.cpp` holding a `bw::LatestValue<BaselineProfile>` (F00's mailbox already solves torn reads). Pump publishes after every step (20 B, trivial at 1 Hz). Before the first publish `profile()` returns false — F06 handles that exactly like its tested absent case (`feature_assembler.cpp:277-288`).

**D5. Poisoning guard (§5 "never learn during suspected events")** *(new decision)*
- **Problem:** no live F09 channel exists; suspected events flow to `IEventSink`.
- **Simplest:** ignore the rule in v1. **Professional:** dedicated live suspected-event flag (needs a new app.h channel — not ours).
- **Choice:** wiring derives a `hold` bool per tick: true while `ctx.alert_status` (absent ⇒ false) reports `level >= Active`, or `ctx.episode_status` (absent ⇒ false) reports `phase != Idle` — both are `bw_core` types in existing mailboxes. Plus a `kPostEventHoldSamples` cooldown after hold clears. Passed as a plain bool in the pure sample: no new channel, no CR, and every input optional.

**D6. Corrupt profile ⇒ fault (§10)**
- CR-031 was accepted at W6 G1: `FaultCode::BaselineCorrupt` = 25, Warning. Discard + restart learning on corrupt/incompatible blobs; wiring tests assert the report. The older draft's F09 reservation note is superseded.

**D7. Pump placement** *(new decision)*
- KV `put()` is synchronous in the caller's task and flash writes stall both cores (`wire_storage.h` StorageTask rationale). **Choice:** the baseline pump runs on **core 0, priority 2** — the most latency-insensitive consumer of core-1 data; inputs cross cores safely through `LatestValue` critical sections. Constants in `wire_baseline.h` per the wiring README convention.

**D8. Review fixes: evidence identity and publication**
- Problem: task cadence alone does not prevent learning twice from held mailboxes; old wear/activity can qualify current windows; a hold during missing sensor data must still arm cooldown. The provider's separate plain bool was also a cross-task data race despite its protected mailbox.
- Simplest: trust producer timing and keep the platform-only pump. Scalable: extract one host-testable cycle, track consumed window stamps, and validate all evidence ages. Choice: `pumpOnce(ctx)` is called by both host tests and the paced task; both PPG and motion stamps must advance before a new pair is consumed, with `kMaxSampleAgeMs` applied to wear/activity and any present state snapshot too. Holds are observed before checking sensor availability. No new thresholds or shared contracts.
- Publication: retain the mailbox and reset/absence semantics, but make its published flag atomic with release/acquire (instead of inventing a second locking mechanism). Reset/rewiring remains startup-only. ENMO must be finite as well as nonnegative before learning.

## 7. Module structure

```
firmware/lib/bw_baseline/
  include/bw/baseline/baseline_types.h           BaselineSample, SampleVerdict, StepResult, TrackerState (pure value types)
  include/bw/baseline/baseline_config.h          BaselineConfig + the §8 named constants
  include/bw/baseline/qualified_sample_filter.h  gate decision (pure, table-driven)
  include/bw/baseline/baseline_tracker.h         BaselineTracker : bw::IBaselineProvider (pure, single-writer)
  include/bw/baseline/baseline_serializer.h      versioned blob codec (pure)
  src/qualified_sample_filter.cpp
  src/baseline_tracker.cpp
  src/baseline_serializer.cpp
firmware/test/bw_baseline/test_bw_baseline/      Unity suite (names in §9)
firmware/src/app/wiring/wire_baseline.{h,cpp}    construction, adapter, pump body, constants
```

API sketch (signatures only; namespaces `bw::baseline` / `f13`):
- `enum class SampleVerdict : uint8_t { Accepted, HoldActive, NotWorn, BadQuality, BadActivity, HrOutOfRange, Outlier };`
- `SampleVerdict QualifiedSampleFilter::check(const BaselineSample&) const;`
- `struct StepResult { bool accepted; SampleVerdict verdict; bool valid_transition; };`
- `StepResult BaselineTracker::step(const BaselineSample&);` — pure, no platform calls, host-testable
- `bool BaselineTracker::profile(BaselineProfile&) const override;` — `IBaselineProvider`; same-task reads in tests, cross-task reads go through the adapter
- `const TrackerState& BaselineTracker::state() const;` / `Result<void> BaselineTracker::restore(const TrackerState&);` — warm restart
- `Result<size_t> BaselineSerializer::serialize(const TrackerState&, uint8_t* buf, size_t cap);` / `Result<void> BaselineSerializer::deserialize(const uint8_t*, size_t, TrackerState&);`

`wire_baseline.h` (wiring README convention, mirrors `wire_wake.h`):
- `void wire(app::AppContext& ctx);` — constructs tracker + adapter, attempts load from `f18::kvStore()` (store absent ⇒ in-memory-only boot, honest), idempotent; README call order, **after** the storage slot.
- `void pumpTask(void* arg);` — task body; `arg` is the AppContext.
- `void pumpOnce(app::AppContext& ctx);` — one host-testable cycle (D8); same implementation as the task uses, no pacing.
- `bw::IBaselineProvider& provider();` — composition-root accessor; the integrator injects it into `wire_wake.cpp:105` (replacing the `nullptr`) at P4.
- Pump contract: read `ctx.ppg_features`, `ctx.motion_features`, `ctx.activity_estimate`, `ctx.wear_estimate`; **skip the tick entirely if any of the four is absent** (never default-construct evidence — a zeroed window reads as Good + 0 g); reject a pair whose windows are older than `kMaxSampleAgeMs` (freshness is the pump's job, F06 precedent); `quality_good = (PpgFeatures.quality==Good && hr_valid && !motion_corrupted && MotionFeatures.quality==Good) && (state_snapshot absent ? true : snapshot.quality.ppg==Good && snapshot.quality.imu==Good)` — this is where the §3 `SourceQuality` input enters; `calibration` from `ctx.state_snapshot` mode when present, else false; `hold` per D5; then `tracker.step()`, publish on change, persist per D3 cadence.

## 8. Constants (named, tagged, justified)

Pump qualification refinement (D8): all four evidence stamps and any present snapshot must be fresh and not future-dated; each accepted/attempted sensor pair is consumed only once. Active alerts/episodes arm the poisoning cooldown even when evidence is absent. This supersedes §7's older PPG/motion-only freshness description.
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMinQualifiedMs` | 1'800'000 (30 min) | `EXPERIMENTAL` | Minimum evidence before `valid=true` (original `kMinQualifiedMinutes`=30, in ms for the u32 counter) |
| `kAdaptationTimeConstantH` | 48 | `EXPERIMENTAL` | Slow drift tracking (original constant) |
| `kEwmaSteadyWindowSamples` | 30'000 | `EXPERIMENTAL` | `DERIVED` from 48 h: ~4 days × ~7200 qualified s/day at 1 Hz; EWMA alpha = 1/this after the ramp |
| `kEwmaRampSamples` | 600 | `EXPERIMENTAL` | 10 min at 1 Hz: incremental-mean seeding (`alpha=1/(n+1)`) converges to the true mean fast; outlier gate off until then |
| `kOutlierBandSigma` | 4.0f | `EXPERIMENTAL` | Reject samples > 4 robust deviations from the current estimate (post-ramp drift guard) |
| `kMinOutlierBandBpm` | 8.0f | `EXPERIMENTAL` | Absolute floor of the outlier band so a near-zero early MAD cannot lock learning out |
| `kMinRestingHrBpm` / `kMaxRestingHrBpm` | 35.0f / 130.0f | `EXPERIMENTAL` | Physiological plausibility band for resting HR; outside ⇒ artifact, not rest |
| `kQualifiedActivity` | {Stationary, Sitting} | `EXPERIMENTAL` | Resting HR requires rest (original constant) |
| `kQualifiedActivityCalibration` | above + {Standing, LyingResting} | `EXPERIMENTAL` | D2 gate widening during guided calibration |
| `kPostEventHoldSamples` | 600 | `EXPERIMENTAL` | 10 min cooldown after a suspected event/episode clears before learning resumes (D5) |
| `kFullConfidenceSamples` | 7'200 | `EXPERIMENTAL` | 2 h of qualified evidence ⇒ `confidence_pct` 100; below, linear ramp |
| `kPersistIntervalMs` | 600'000 (10 min) | `EXPERIMENTAL` | Flash-write cadence (~144 writes/day, trivial wear); also on valid false→true transition |
| `kProfileVersion` | 1 | `DERIVED` | `BaselineProfile::version` = meaning-version of the profile; mirrors blob schema v1 |
| `kBlobMagic` | 0x42313346 ('F13B') | `DERIVED` | Corruption/format detection in the 32 B blob (D3) |
| `kBlobSchemaV1` / `kBlobBytes` | 1 / 32 | `DERIVED` | Blob layout version and size (fits §11 < 200 B) |
| `kStoreKey` | `"f13.baseline"` | `DERIVED` | Stable IKeyValueStore key; feature-prefixed, never renumbered |
| `kMaxSampleAgeMs` | 5'000 | `EXPERIMENTAL` | Pump freshness guard: PPG/motion windows older than 5 s are stale, not evidence |
| `kBaselinePumpPeriodMs` | 1'000 | `DERIVED` | One tick per F02 window (1 Hz grid); faster polling duplicates samples |
| `kBaselinePumpStackBytes` | 2'048 | `EXPERIMENTAL` | Sample (~40 B) + profile + FreeRTOS frames; matches other pumps; re-measure at Validate |
| `kBaselinePumpPriority` | 2 | `DERIVED` | ≤ 3 wave convention; below F06's pump (3) — the learner is background |
| `kBaselinePumpCore` | 0 | `DERIVED` | D7: synchronous KV flash writes must stay off the real-time core |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan (host, Unity, `firmware/test/bw_baseline/`)
1. `test_profile_absent_before_any_data` — first call: `profile()` false; no fabricated values.
2. `test_learning_converges_on_qualified_stream` — 1800 scripted 1 Hz sitting/worn/good windows at 60 bpm/0.05 g ⇒ valid at `kMinQualifiedMs`, `|resting_hr_bpm − 60| < 0.5`, enmo within 0.01.
3. `test_adaptation_tracks_drift` — 60 bpm steady, step to 72 ⇒ estimate approaches monotonically, within tolerance after `kEwmaSteadyWindowSamples`, no overshoot.
4. `test_outlier_rejection_and_band_floor` — steady stream + single 120/30 bpm spikes ⇒ shift < epsilon; boundary rows exactly at `max(kMinOutlierBandBpm, kOutlierBandSigma·mad)` (enter/exit, no flapping).
5. `test_not_worn_windows_excluded` — Worn→NotWorn→Worn; NotWorn samples leave estimates bit-identical.
6. `test_poor_quality_windows_excluded` — Degraded/Insufficient, `hr_valid=false`, `motion_corrupted=true` ⇒ no update (§5 rule 1).
7. `test_activity_gate_and_calibration_widening` — Walking/Running excluded; Standing excluded in normal mode, accepted with `calibration=true`; mode-absent (false) = normal gate (D2).
8. `test_poisoning_hold_and_cooldown` — `hold=true` ignored; cooldown `kPostEventHoldSamples` after clear ignored; learning resumes after (§5 rule 2).
9. `test_hr_plausibility_clamp` — 200/20 bpm rejected; 35/130 accepted (boundary rows).
10. `test_serializer_round_trip` — state → bytes → state, bit-exact floats.
11. `test_serializer_rejects_corrupt` — flipped bits in magic/payload/crc ⇒ `Err::Corrupt`; wrong schema ⇒ `Err::Unsupported`; tracker `restore()` refuses ⇒ discard + restart learning (§10).
12. `test_serializer_truncated_or_oversized` — short buffer ⇒ `Corrupt`; `cap < kBlobBytes` ⇒ `Err::Full`, nothing written.
13. `test_warm_restart_keeps_state` — serialize valid state, new tracker, restore ⇒ profile immediately valid, alpha stays at steady (no re-ramp), learning continues.
14. `test_confidence_ramp` — confidence at `kMinQualifiedMs` and at `kFullConfidenceSamples`; never > 100; `valid` implies ≥ minimum.
15. `test_adapter_publish_semantics` — adapter `profile()` false before first publish, true after; mirrors what F06's `FakeBaseline` consumer expects (absent vs valid).
16. `test_state_sizes_fit_budget` — `sizeof` assertions: tracker + filter state ≤ 256 B; blob == 32 B; profile == 20 B (cross-checks the F00 contract pin).
17. `test_time_gap_and_reboot_boundary` — huge `t_ms` jumps (uint64-scale) never crash or wrap logic (v1 state is time-free per sample); persisted state contains no `TimeMs` (reboot-safe).
18. `test_store_absent_boots_in_memory` — wiring-level fake: no `IKeyValueStore` ⇒ tracker starts empty, learns, runs (failure behavior below).

## 10. Failure behavior
- Corrupt / version-incompatible stored profile ⇒ `restore()` refuses, tracker restarts learning (`valid=false` until `kMinQualifiedMs` again); fault report `BaselineCorrupt` via accepted CR-031 (D6).
- `IKeyValueStore` absent or `put` failing ⇒ keep running in memory; persist retried next interval; never blocks learning.
- All four input mailboxes empty (sensors dead) ⇒ pump skips ticks; profile eventually stops changing; `profile()` keeps last honest answer (no decay in v1, §13).

## 11. Budget
- Static state (tracker + filter + serializer + adapter): ≤ 256 B — asserted by test 16; spec §11 RAM < 1 KB ✓ (adapter adds the 20 B profile + `LatestValue` overhead).
- Persisted blob = 32 B — §11 flash < 200 B ✓.
- CPU: one O(1) step per 1 Hz tick, no loops, no allocation — negligible; pump stack 2 048 B, re-measured at Validate.

## 12. Out of scope
- Population norms.
- Sleep schedule inference / time-of-day buckets (later, needs wall time; additive change).
- Stability-based confidence and wall-clock staleness decay (§13).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Baseline absent/present both exercised in F06 integration test (fake provider).

---
