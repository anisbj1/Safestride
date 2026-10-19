# F07 — State engine

> Track **C** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Deterministic state machine composing wear, activity, quality and alertness probabilities into a stable `StateSnapshot`: smoothing, persistence, hysteresis, minimum durations, recovery, unknown/not-worn handling and sensor-quality constraints.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `AlertnessProbabilities` (incl. its `reasons` mask, CR-010, merged per D14),
  `WearEstimate`, `ActivityEstimate`, `SourceQuality`
- `OperatingMode`/`PowerProfile` (from F24 provider; may be defaults)

**Outputs (produced contracts)**
- `StateSnapshot` (`LatestValue`, 1 Hz)
- State-transition records (internal event for F17 via `IEventSink`; types, fields and
  which transitions emit one: **D12**, table in §7)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Trigger hardware or alerts directly.
- Read raw sensors or model internals.
- Embed magic thresholds outside `StateEngineConfig`.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**CR-034 targeted forwarding (developer accepted 2026-10-19).** Forward F06's
`wake_motion` unchanged only with gates open, a probabilistic rung, and usable
IMU. Do not refresh its timestamp on new StateSnapshots. The simpler reason-bit
alternative cannot prevent cached-input replay; a separate evidence stream
would add needless plumbing. The small appended evidence value changes no rung
thresholds, sleep dwells, events, or confidence calculations. Contract rationale
and compatibility are in CR-034; host tests cover original timestamps and gates.

**D1. Implementation style**
- Simplest: Nested if/else with counters.
- Professional/scalable: Table-driven transitions with named guards and a config struct.
- Trade-offs: Table adds indirection but makes rules reviewable and testable per transition.
- **Choice:** Enum state + explicit `evaluateXxx()` functions per state, all thresholds in `StateEngineConfig`; transition table documented in the spec (§7). Table-driven code generation is rejected as over-engineering.

**D2. Smoothing**
- Simplest: Use raw probabilities.
- Professional/scalable: EMA + entry/exit thresholds + minimum duration.
- Trade-offs: EMA adds lag, prevents flapping.
- **Choice:** EMA (alpha in config) followed by enter/exit thresholds with duration counters.

**D3. Precedence of gates**
- Simplest: Ad hoc.
- Professional/scalable: Fixed priority: NotWorn > Insufficient data (Unknown) > probabilistic states.
- Trade-offs: Fixed order is less flexible but predictable.
- **Choice:** Fixed order; documented and tested. A gate is only *asserted* on a fresh
  estimate: a stale `WearEstimate` may not claim `NotWorn` (it falls to gate `Unknown`).

**D4. Where time comes from**
- Problem: dwells, staleness checks and `in_state_ms` all need "now".
- Simplest: pass `t_ms` inside `StateInputs` (time as data).
- Professional/scalable: the engine holds a `const IClock&` and reads `nowMs()` itself.
- Trade-offs: an injected clock saves one field but makes `step()` no longer a pure
  function of its argument (01 §1 rule 5), forces every host test to build a clock
  fixture, and a late `nowMs()` read silently distorts dwell arithmetic. Time-as-input
  makes a test a plain table of `(t_ms, inputs)` rows and keeps "read the clock" in one
  place (the 1 Hz pump).
- **Choice:** time as input. `FakeClock` is used where the pump lands (wiring stage),
  not inside the engine.

**D5. Staleness limits are per producer**
- Problem: §10 needs "no input ⇒ Unknown", but producers run at different rates
  (runtime §3.2: wear/activity at 1 Hz; §3.3: alertness epoch aggregation at 30 s).
- Simplest: one `kInputStaleMs` for every input (the literal §10 wording).
- Professional/scalable: one named limit per producer class, plus the contract's own
  validity flag (`AlertnessProbabilities::valid`, 02 §1).
- Trade-offs: one constant is one line but is wrong for either producer — sized for
  1 Hz it trips on every late epoch, sized for the 30 s epoch a dead IMU goes
  unnoticed for half a minute.
- **Choice:** two limits — `kInputStaleMs` (wear, activity) and `kAlertnessStaleMs`
  (alertness, with `valid` as the primary signal). `[ASSUMED]` F06 publishes
  `AlertnessProbabilities` at least every 30 s (verify against F06 when it lands).

**D6. Gates enter fast and leave slow (asymmetric dwells)**
- Problem: what "quality dwell" (§9) means, how fast to enter/leave `NotWorn`/`Unknown`.
- Simplest: symmetric dwell `kQualityDwellMs` on entry *and* exit, plus a separate
  `kWearRecoveryDwellMs` counter for re-wearing.
- Professional/scalable: gates are entered **immediately** (an honest "no reliable
  data" / "not worn" claim must never wait), and *every* evidence timer is reset at
  gate entry and only advances while the gate condition is clear — so every exit pays
  a dwell using counters that already exist.
- Trade-offs: symmetric dwell delays the honest answer by `kQualityDwellMs` and costs
  two extra counters. Separate quality/re-wear exit counters are **rejected as dead
  logic**: they are subsumed by `kAwakeRecoveryMs` and the rung dwells (all ≥ 30 s >
  5 s), i.e. a magic number with no observable effect.
- **Choice:** immediate entry; reset-and-advance exits. The spec's "exits only after
  quality dwell" is realized as: *timers reset at gate entry and advance only while
  the gate condition is clear* ⇒ leaving `Unknown` costs ≥ `kAwakeRecoveryMs` (calm) or
  the target rung's dwell. While gate `NotWorn` holds the snapshot reads `NotWorn`
  whatever the quality (D3); when that gate clears the state moves to `Unknown` first,
  never straight to `Awake`. **Entry is detected as the rising edge of
  `gate = G1 || G2`, not as a state change** (auditor A-01, FIX 2026-10-11): a gate
  blip while the engine is already in a gate state (e.g. one tick of
  both-sources-`Insufficient` while `Unknown`) still declares the window unusable and
  must wipe the evidence timers — otherwise the calm/rung dwell would straddle an
  untrusted window and commit on pre-gate evidence (premature `Awake`/`Drowsy`).
  Pinned by `test_gates_quality_blip_while_unknown_resets_recovery_dwell`.

**D7. Evidence channels — what the thresholds see**
- Problem: three probabilities feed four states; which smoothed quantity is thresholded
  by which rung.
- Simplest: one scalar (`drowsy` alone, or `1 - awake`) for the whole ladder.
- Professional/scalable: one EMA per class (3 channels), each rung thresholding its own
  probability.
- Trade-offs: one scalar is minimal but lets "very drowsy" read as sleep-like, so
  `PossibleSleep` would be claimed from drowsy evidence; three channels are general
  but the wake channel is redundant — with normalized probabilities
  `evidence_drowsy < exit` already implies wakefulness ≥ 1 − exit, and EMA is linear,
  so `EMA(x+y) = EMA(x) + EMA(y)` for equal alpha and seed.
- **Choice:** two channels — `evidence_drowsy = EMA(drowsy + sleep)` (feeds Drowsy and
  HighDrowsinessRisk) and `evidence_sleep = EMA(sleep)` (feeds PossibleSleep only).
  Each is **seeded on the first valid sample** (no warm-up lag from 0.0) and reset at
  gate entry (D6) so recovery is judged from clean data only.

**D8. Hysteresis and dwell rules (the ladder)**
- Problem: thresholds alone flap; dwell alone still commits on a single spike.
- Simplest: one threshold per state + minimum time in state.
- Professional/scalable: an enter/exit pair per rung, dwell on escalation only,
  immediate de-escalation (a full table-driven transition engine was already rejected
  in D1; this is the same rule set, expressed as four rules).
- Trade-offs: one threshold per state flaps at the boundary (fails §9); per-rung pairs
  double the threshold count but make every boundary testable and every excursion
  bounded by a dwell.
- **Choice:** four rules, applied in this order:
  - **E1 escalate** — `target` = highest rung whose **enter** threshold is met; commit
    only after `runMs(target) ≥ dwell_target`. The counter **resets** (not pauses) on
    any sample below enter ⇒ evidence must be *continuous* ("sustained", §8).
  - **E2 de-escalate** — `supported` = highest rung whose **exit** threshold is met;
    if `supported < current`, move there **immediately**, no dwell (de-escalation is
    the safe direction; the asymmetric dwell already prevents flapping back).
  - **E3 recover** — if no rung is supported and `evidence_drowsy < kDrowsyExitProb`
    (calm) for ≥ `kAwakeRecoveryMs` → `Awake`. This is also the exit rule for gate
    states (D6), so `Unknown → Awake` needs the same sustained calm.
  - **E4 hold** — everything else: stay, `in_state_ms += dt`.
  - Consequence `[ASSUMED]`: ambiguous but stable evidence (e.g. `evidence_drowsy`
    stuck at 0.50) stays `Unknown` with reason `InsufficientEvidence` rather than
    guessing `Awake` — 00 §1 makes "unknown" a first-class output. Flagged for the
    Design review (§13).

**D9. Confidence and reason codes**
- Simplest: copy `AlertnessProbabilities.confidence`; reasons `0` for probabilistic
  states.
- Professional/scalable: compose the independent axes multiplicatively and always
  populate `ReasonMask` from a documented set.
- Trade-offs: copying over-reports (a confident model on an unworn device), a full
  probabilistic fusion is over-engineering for a prototype; an empty mask leaves F22
  with nothing to explain for the *main* claim.
- **Choice:** `Unknown → 0`, `NotWorn → wear.confidence`, ladder →
  `clamp01(alertness.confidence × wear.confidence × degraded_factor)`. `Degraded`
  lowers confidence and ORs `LowSignalQuality` but never forces `Unknown` (only
  `Insufficient` does — 00 §1 gating is about *unusable* data). A low model confidence
  does **not** gate (no new threshold; F10/F12 apply their own floors). **CR-008 was
  accepted and applied 2026-10-11** (commit `697da91`): `SustainedLowAlertnessProbability`
  (bit 12), `SustainedWakefulnessProbability` (bit 13), `InsufficientEvidence`
  (bit 14) exist in `bw_core/include/bw/state.h` — implement with the real codes; the
  gate-bits-only fallback is retired. Bit assignment: `Drowsy`/`HighDrowsinessRisk`/
  `PossibleSleep` OR bit 12 (`SustainedLowAlertnessProbability`); `Awake` entered via
  E3 ORs bit 13 (`SustainedWakefulnessProbability` — sustained calm, not a guess);
  ambiguous-stable `Unknown` (E4) carries bit 14 (`InsufficientEvidence`); gate
  `Unknown`/`NotWorn` carry their gate bits (`SensorUnavailable`, `LowSignalQuality`,
  `DeviceNotWorn`). How F06's own reason bits merge in: **D14**.

**D10. Activity, mode and power: mirror, do not re-decide**
- Simplest: mirror the three axes into the snapshot (stale activity → `Unknown`).
- Professional/scalable: an activity veto on the ladder (e.g. no `PossibleSleep` while
  `Running` with high confidence).
- Trade-offs: the veto needs a confidence floor + a context set, and is a *second*
  opinion that can contradict F06 — which already consumes motion features — with no
  way to tune the two sides independently (double counting, 01 §1 separation).
- **Choice:** mirror only. `mode`/`power` come from the inputs (defaults `Day`/
  `Normal` until F24 exists — §3), and F07 does not change behavior by mode;
  thresholds would arrive through `StateEngineConfig` if ever needed. F07's
  "composition" (§1) means *one coherent snapshot with fixed precedence*, not a second
  classifier. If replay (F29) shows contradictory snapshots, a configurable veto can
  be added to `StateEngineConfig` later.

**D11. How a transition record reaches F17**
- Simplest: the engine holds an `IEventSink*` and publishes inside `step()`.
- Professional/scalable: `step()` returns the record; the pump publishes it.
- Trade-offs: sink injection gives a single call site but makes `step()` depend on an
  I/O object, every test then needs a fake sink, and an `Err::Full` from F17's
  append-only log would have to be handled *inside* the state machine — a storage
  failure could perturb alertness state, which is the wrong coupling direction.
- **Choice:** return the record: `StateOutputs { snapshot, transitioned, previous,
  event_valid, event }`. The pump calls `IEventSink::publish`; a rejected publish is
  the pump's problem (drop/log), never the engine's. This also keeps wiring
  logic-free (01 §1 rule 3): it only forwards. `seq`/`boot_id` stay `0` (F17
  assigns), `t_start_unix = 0` (no wall clock yet, 01 §4).

**D13. Invalid probabilities**
- Simplest: clamp into `[0,1]` and use them.
- Professional/scalable: reject the input (→ gate `Unknown`) and report a fault
  through `IFaultReporter` (F26).
- Trade-offs: clamping turns an upstream bug into a plausible-looking state — 00 §1
  forbids fabricating a claim from bad data; and NaN makes every comparison false,
  which would pin a rung "supported" forever (E2 would never de-escalate). A fault
  report, however, belongs to F23/F26, not to the decision layer.
- **Choice:** validate — any non-finite or out-of-range probability ⇒ the alertness
  input is treated as invalid (gate `Unknown`, reason `SensorUnavailable`), never
  silently clamped. `evidence_drowsy` saturates at `kMaxProbability` after validation.
  Fault reporting is out of scope (F23/F26).

**D14. CR-010 pass-through of `AlertnessProbabilities::reasons`**
- Problem: CR-010 (accepted + applied 2026-10-11, commit `697da91`) appended a trailing
  `ReasonMask reasons` field to `AlertnessProbabilities` — the evidence F06 saw while
  computing the epoch. `StateSnapshot.reasons` is the one explainability field
  F10/F12/F22 consume; decide how the two masks combine.
- Simplest: ignore F06's mask; the snapshot carries only F07's own bits (D9).
- Professional/scalable: OR F06's mask through for exactly the states whose claim is
  built from that alertness evidence (the ladder states); a third option, a per-bit
  whitelist/map from F06 codes to snapshot codes, was considered.
- Trade-offs: dropping F06's bits loses the evidence trail behind the main claim and
  forces F22 to read two sources that can disagree; blanket pass-through into *every*
  state would let alertness-axis evidence decorate gate states (`NotWorn`, `Unknown`)
  whose claim the alertness axis did not produce; a whitelist is a maintenance table
  that silently drops future append-only codes (bits 18+ are free and will be used).
- **Choice:** extend D9 — for ladder states (`Awake`, `Drowsy`, `HighDrowsinessRisk`,
  `PossibleSleep`) `snapshot.reasons = alertness.reasons | <F07's own bits per D9>`;
  gate states keep gate bits only and never inherit F06's mask (a gate says the
  alertness evidence is unusable or out of scope). `Degraded` keeps both: the
  `LowSignalQuality` bit is added and the pass-through is retained. A rejected, stale
  or invalid alertness input (G2, D13) yields no pass-through at all. The transition
  record (D12) copies the merged snapshot mask, so F17/F22 see one coherent
  explanation per claim. No new constant: a pure OR, no threshold, no whitelist.

**D15. Where the 1 Hz pump logic lives**
- Problem: §15 originally placed the whole pump (sequence-gated pulls, quality
  derivation, engine step, publish/drop) inside `wire_state.cpp`, so no host test
  could drive it — the auditor found the wiring latched a stopped F02's last
  `Good` quality forever, with no test to catch it (A-03).
- Simplest: keep the pump in the wiring and skip the pump test (wiring is not in
  the native build; it pulls in `app.h`/FreeRTOS).
- Professional/scalable: extract the per-tick logic into `bw::state::StatePump`
  (pure logic: ports as non-owning pointers, `tick(TimeMs now)` — time passed in,
  D4) and leave `wire_state.cpp` the platform shell only (clock read,
  `vTaskDelayUntil`, task creation, port binding); drive `StatePump` from the
  native suite with a `FakeClock` harness.
- Trade-offs: the extraction moves ~80 lines out of the wiring (a small spec §15
  restructure), but it makes the pump host-testable per 01 §1 rule 5, keeps the
  wiring logic-free (rule 3), and costs no contract change — `PumpPorts` uses
  only bw_core types. Skipping the test was rejected: an untested pump already
  shipped one staleness bug.
- **Choice:** `StatePump` in `bw_state`; `wire_state.{h,cpp}` is the shell; the
  `test_pump.cpp` FakeClock harness pins the tick, the feature-quality staleness
  aging (`kFeatureQualityStaleMs`) and D11 publish/drop behaviour.

| Path | Contents |
|---|---|
| `include/bw/state/state_config.h` | §8 constants + `StateEngineConfig` (every threshold a field, defaulting to the constant) |
| `include/bw/state/state_types.h` | `StateInputs`, `StateOutputs` — trivially copyable value types (02 §1) |
| `include/bw/state/smoother.h` | `Smoother` — EMA: `step(float)`, `reset()`; seeds on first sample |
| `include/bw/state/duration_counter.h` | `DurationCounter` — `update(bool, TimeMs)`, `runMs(TimeMs)`, `sustained(TimeMs, TimeMs)`, `reset()`; resets (never pauses) when inactive; saturating |
| `include/bw/state/state_engine.h` | `StateEngine`, `StateEngineConfig`, `StateProvider : IStateProvider` |
| `src/state_engine.cpp` | gate evaluation + ladder + snapshot/record construction (table below) |
| `include/bw/state/state_pump.h` + `src/state_pump.cpp` | `StatePump` + `PumpPorts` — the §15 1 Hz pump as pure logic (**D15**): sequence-gated mailbox pulls, feature-quality staleness aging (`kFeatureQualityStaleMs`, A-03), engine step, snapshot/record publishing, drop counting; optional F23 quality-provider port (**D16**, P4b) |
| `src/app/wiring/wire_state.{h,cpp}` | construction + ctx-side port binding + `StateProvider` + the 1 Hz `StateTask` shell around `bw::state::StatePump` (runtime §3.2; clock read + `vTaskDelayUntil` only) + the P4 seams `attachMailboxes` / `attachEventSink` / `attachQualityProvider` (see §15); task constants in §8 |
| `test/bw_state/test_bw_state/` | `main.cpp`, `test_smoother.cpp`, `test_duration_counter.cpp`, `test_gates.cpp`, `test_ladder.cpp`, `test_snapshot.cpp`, `test_events.cpp`, `test_pump.cpp` |

```cpp
namespace bw::state {
struct StateInputs {                      // assembled by the 1 Hz pump; time as data (D4)
    TimeMs t_ms;
    AlertnessProbabilities alertness;     // bw/model.h
    WearEstimate wear;                    // bw/context.h
    ActivityEstimate activity;
    SourceQuality quality;                // bw/quality.h
    OperatingMode mode;                   // F24 via the pump; defaults until then (D10)
    PowerProfile power;
};
struct StateOutputs {
    StateSnapshot snapshot;               // published to LatestValue<StateSnapshot>
    bool transitioned;                    // alertness axis changed at this step
    AlertnessState previous;
    bool event_valid;                     // a record must be published (D11)
    Event event;                          // fully built; publish as-is
};
class StateEngine {
 public:
    explicit StateEngine(const StateEngineConfig& cfg = StateEngineConfig{});
    StateOutputs step(const StateInputs& in);   // pure, no allocation, no platform calls
    AlertnessState state() const;               // introspection for wiring/tests
};
}  // namespace bw::state
```

### Transition table (normative — §14 requires tests to mirror it row by row)

Gates are evaluated first, in order (D3); ladder rules E1–E4 (D8) run only when no
gate holds. "reset" = re-seed both smoothers, zero every counter (D6/D7).

| # | Current | Condition (top-down) | Next | Side effects |
|---|---|---|---|---|
| G1 | any | fresh `WearEstimate` with `state == NotWorn` | `NotWorn` | reset; `DeviceNotWorn` record if current ≠ `NotWorn` |
| G2 | any, except when G1 holds | wear or activity older than `kInputStaleMs`, or `alertness.valid == false`, or alertness older than `kAlertnessStaleMs`, or invalid probabilities (D13), or `alertness.input_quality == Insufficient`, or **both** sources `Insufficient` | `Unknown` | reset; no record; reason `SensorUnavailable`/`LowSignalQuality` |
| G3a | `NotWorn` | G1 no longer holds | `Unknown` | reset; no record (never straight to `Awake`, D6) |
| G3b | `Unknown` | gate clear | stays until E1/E3 | smoothers and counters start advancing |
| E1 | any | highest rung with `enter` met, held ≥ its dwell | that rung | `DrowsinessEvent` if target ∈ {`Drowsy`,`HighDrowsinessRisk`} and ≠ current |
| E2 | `Drowsy`/`HighDrowsinessRisk`/`PossibleSleep` | current rung's `exit` not met | highest rung whose `exit` is met, else E3 | immediate, no dwell |
| E3 | any (incl. gate states) | no rung supported && calm ≥ `kAwakeRecoveryMs` | `Awake` | no record |
| E4 | any | otherwise | stays | `in_state_ms += dt` (saturating) |

Bands (nested, so every de-escalation lands on a rung that is already "supported"):
`0.40 (Drowsy exit) < 0.60 (Drowsy enter)`, `0.40 < 0.55 (HighRisk exit) < 0.60 < 0.75
(HighRisk enter)`, `0.55 < 0.65 (Sleep exit) < 0.75 < 0.85 (Sleep enter)` — and
`sleep ≥ 0.85 ⇒ drowsy+sleep ≥ 0.85 ≥ 0.75`, so `PossibleSleep` is always reachable
from a state `HighDrowsinessRisk` also supports.

## 8. Constants (named, tagged, justified)
Defaults live in `StateEngineConfig`; tests and later F18 settings override fields,
never the constants (F03-style config, 00 §7.5).

| Name | Value | Tag | Why |
|---|---|---|---|
| `kDrowsyEnterProb` | 0.60 | `EXPERIMENTAL` | Hysteresis gap (spec) |
| `kDrowsyExitProb` | 0.40 | `EXPERIMENTAL` | Hysteresis gap (spec); also the "calm" line of rule E3 |
| `kHighRiskEnterProb` | 0.75 | `EXPERIMENTAL` | Escalation must sit above Drowsy's enter; 0.20 gap matches the Drowsy gap |
| `kHighRiskExitProb` | 0.55 | `EXPERIMENTAL` | Lands inside Drowsy's band [0.40, 0.60) so E2 de-escalates into Drowsy; gap 0.20 |
| `kSleepEnterProb` | 0.85 | `EXPERIMENTAL` | `sleep` class alone must dominate; implies `drowsy+sleep ≥ 0.75` so the ladder stays nested |
| `kSleepExitProb` | 0.65 | `EXPERIMENTAL` | Above HighRisk's exit, so E2 de-escalates into HighRisk; gap 0.20 |
| `kDrowsyMinDurationMs` | 60000 | `EXPERIMENTAL` | Ignore brief dips |
| `kHighRiskMinDurationMs` | 120000 | `EXPERIMENTAL` | Escalation needs sustained evidence |
| `kPossibleSleepMinDurationMs` | 180000 | `EXPERIMENTAL` | 3 min sustained sleep-like evidence before claiming PossibleSleep |
| `kAwakeRecoveryMs` | 30000 | `EXPERIMENTAL` | Recovery must be sustained; also the gate-exit dwell (D6) |
| `kEmaAlpha` | 0.2 | `EXPERIMENTAL` | ≈ 5 s time constant at 1 Hz |
| `kInputStaleMs` | 5000 | `EXPERIMENTAL` | Wear/activity are 1 Hz (runtime §3.2); 5 missed windows with task-lateness slack ⇒ `Unknown` (§10) |
| `kAlertnessStaleMs` | 60000 | `DERIVED` | 2 × 30 s epoch (runtime §3.3): one late epoch is tolerated, two are not; `valid` is the primary signal (D5) |
| `kFeatureQualityStaleMs` | 5000 | `EXPERIMENTAL` | The pump ages the MotionFeatures/PpgFeatures quality channel back to `Insufficient` after 5 missed 1 s windows (same rationale as `kInputStaleMs`): a stopped F02 must not leave its last `Good` quality latched (missing beats a guess, 00 §1; auditor A-03) |
| `kDegradedConfidenceFactor` | 0.75 | `EXPERIMENTAL` | A quarter off per degraded source; two degraded sources ⇒ 0.56 — lower, never zero, so `Degraded` never looks like `Unknown` |
| `kMaxProbability` | 1.0f | `DERIVED` | Upper bound of 02 §1's `[0,1]` range; saturates `drowsy+sleep` (D13) |
| `kStateFeatureId` | 7 | `DERIVED` | This feature's id, written to `Event.detector_id` so a decoder tells a state record from a detector record (F10's `kAlertFeatureId` convention) |
| `kStatePayloadPrevIndex` | 0 | `DERIVED` | `Event.payload[0]` = previous `AlertnessState` (D12) |
| `kStatePayloadNewIndex` | 1 | `DERIVED` | `Event.payload[1]` = entered `AlertnessState` (D12) |
| `kStatePayloadReservedBytes` | 6 | `DERIVED` | `payload[2..7] = 0`: reserved, and a zero tail lets a decoder stop at the last field it knows |
| `kMaxDurationFieldMs` | 0xFFFFFFFFu | `DERIVED` | `in_state_ms`/`duration_ms` are uint32: saturate instead of wrapping past 49 days |
| `kStateTaskPeriodMs` | 1000 | `DERIVED` | runtime §3.3: state decisions at 1 Hz; the pump period that assembles `StateInputs` |
| `kStateTaskPriority` | 3 | `DERIVED` | ≤ F03's intelligence pump (3): the consumer must never preempt the producers it reads (runtime §3.2 "med"); Alert/Input tasks run strictly higher so alerting is unaffected |
| `kStateTaskStackBytes` | 3072 | `EXPERIMENTAL` | pure-logic step, no large stack arrays; measure with `uxTaskGetStackHighWaterMark` at Validate and trim if >1 KB spare |
| `kStateTaskCore` | 1 | `DERIVED` | decision layer runs on core 1 with the real-time loop (runtime §3.2) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host only: `pio test -e native`, Unity, deterministic. Time is an **input** (D4), so
engine tests drive `(t_ms, inputs)` timelines directly — no clock object, no sleeps;
the wiring has landed and the pump logic moved into `bw_state` (**D15**, A-03 fix), so
`FakeClock` drives the `test_pump.cpp` pump harness (the harness reads
`clock.nowMs()` and feeds it to `tick()` — the clock stays outside the logic, D4). Every §7 table
row has at least one test; add a row per bug found.

| File | Cases |
|---|---|
| `test_smoother.cpp` | first sample seeds (no warm-up from 0); alpha applied per step; `reset()` re-seeds; NaN/±inf rejected upstream never reach it (D13) |
| `test_duration_counter.cpp` | accumulates while active; **resets** on inactive (never pauses); `sustained()` inclusive at the boundary; saturates at `kMaxDurationFieldMs`; stale/future `t_ms` (`elapsedMs`) never underflows |
| `test_gates.cpp` | (G1) boot with no input → `Unknown` + `SensorUnavailable`, confidence 0; (G1) `NotWorn` + `Insufficient` → `NotWorn` (precedence); (G2) `Worn` + `Insufficient` → `Unknown` + `LowSignalQuality`; (G2) exactly `kInputStaleMs` counts as fresh, one ms more → `Unknown`; (G2) `valid == false`, `kAlertnessStaleMs` exceeded; (G2) one source `Insufficient` alone does *not* gate; (G3a) `NotWorn → Worn` goes to `Unknown` first, never `Awake`; (E3) quality restored + calm — `test_gates_quality_blip_while_unknown_resets_recovery_dwell` (auditors A-01/A-02: a G2 gate **re-asserted while `Unknown`**, then cleared): `Unknown` at 29 s after the restore, `Awake` at a **fresh** `kAwakeRecoveryMs`; flapping `NotWorn ↔ Worn` every 3 s **follows the gate in both directions** (G1/G3a are both instantaneous — the anti-flapping protection is the ladder dwell; corrected 2026-10-11, the old "stays `NotWorn`" wording would sit on evidence that already changed) and flapping `Worn ↔ Uncertain` never claims `NotWorn` and leaves `Unknown` ≤ once per `kAwakeRecoveryMs` |
| `test_ladder.cpp` (table-driven, one row per rule) | E1: 0.65 → `Drowsy` exactly at 60 s, not at 59 s; 0.59 never enters; exactly `kDrowsyEnterProb` counts; a dip below enter at 59 s resets the counter; 0.80 for 120 s → `HighDrowsinessRisk`, 0.70 for 10 min stays `Drowsy`; `sleep ≥ 0.85` for 180 s → `PossibleSleep`, high `drowsy` alone never claims it; (E2) `HighRisk` at 0.80 → 0.50 ⇒ `Drowsy` immediately, no dwell; `PossibleSleep` with `sleep` 0.60 ⇒ `HighDrowsinessRisk`, while `sleep` 0.70 **holds** `PossibleSleep` (inside the hysteresis band 0.65–0.85 — corrected 2026-10-11: 0.70 ≥ the 0.65 exit, so E2 does not fire; D8's exit semantics are normative); (E3) `Drowsy` → calm 0.39 for 29 s stays, 30 s → `Awake`; 0.45 holds `Drowsy` indefinitely (hysteresis); (E4/E3) stable ambiguous 0.50 after a gate → stays `Unknown` + `InsufficientEvidence`; E1 from a gate state requires the full rung dwell, not merely a clear gate |
| `test_snapshot.cpp` | mirrors `mode`/`power`/`activity`/`quality`; stale activity → `ActivityContext::Unknown` (D10); confidence: `Unknown` = 0, `NotWorn` = `wear.confidence`, ladder = clamped product, one `Degraded` source × `kDegradedConfidenceFactor` + `LowSignalQuality` bit; `in_state_ms` resets on transition and saturates; invalid probability (NaN, 1.5) → `Unknown` + `SensorUnavailable` (D13), never a clamped guess; **(D14)** pass-through: ladder state ORs `alertness.reasons` (e.g. bits 2+1) with F07's own bits; `NotWorn`/`Unknown` never inherit F06's bits; empty F06 mask → F07 bits only; stale/invalid alertness (G2/D13) → no pass-through; `Degraded` adds `LowSignalQuality` *and* keeps the pass-through |
| `test_events.cpp` | one `DrowsinessEvent` on entering `Drowsy` **and** on entering `HighDrowsinessRisk` (none on later steps); one `DeviceNotWorn` on entering `NotWorn`; none for `Unknown`/`Awake`/`PossibleSleep` and none at construction; field semantics per D12 (`duration_ms` = duration of the state left, `detector_id == kStateFeatureId`, payload prev/new, `seq`/`boot_id`/`t_start_unix` == 0); `transitioned`/`previous` flags; record `reasons` equal the merged snapshot mask at the transition (D12 + D14) |
| `test_pump.cpp` (D15, auditor A-03) | FakeClock pump harness: the 1 Hz tick drives boot-`Unknown` → `Awake` exactly at `kAwakeRecoveryMs` with exactly one transition, `lastRunMs` tracking the clock (A-03(1)); stalled `motion`/`ppg` mailboxes age to `Insufficient` at `kFeatureQualityStaleMs` (boundary: exactly at the limit still `Good`, one tick more is stale) and gate G2 → `Unknown` + `LowSignalQuality` while wear/activity/alertness stay fresh (A-03(2) — fails if the aging is reverted); D11 publishing: a `DeviceNotWorn` record is delivered to an attached sink (type + `detector_id` intact), an absent sink counts it `droppedEvents()`, a rejected publish (`Err::Full`) is counted dropped too — never blocking; no record for `Unknown`/`Awake`; **P4b quality provider (D16, +7)**: a bound provider `Insufficient` on BOTH axes gates `Unknown` + `LowSignalQuality` while the feature mailboxes publish `Good` (and one `Insufficient` provider axis alone does NOT gate, §10), provider `Good` keeps the engine `Awake` while the feature mailboxes are STALLED (substitution beats stale derivation), provider silence ages to `Insufficient` exactly at `kFeatureQualityStaleMs` (boundary inclusive, then G2), a never-received provider ⇒ both axes `Insufficient` despite healthy features, the RECOVERY half — provider `Insufficient` gates, provider `Good` (with the feature mailboxes STALLED the whole time, so only the provider can lift the gate) pays the FULL `kAwakeRecoveryMs` back to `Awake`, exactly at it, then one `Degraded` provider axis discounts the confidence (D9) + sets the `LowSignalQuality` bit without gating (FIX loop 1), a provider gate MID-RUNG (drowsy evidence to `Drowsy` via the alertness mailbox, `ema_alpha = 1.0` test injection) requires the FULL rung dwell after the gate falls (D6 rising-edge reset through the provider path), and a provider returning `true` WITHOUT writing `out` reads `Insufficient`/`Insufficient` — the pump's defensive out-param seed, never the zero-initialized `Good` (00 §1) - each must FAIL when the provider block in `tick()` is disabled (H10), and the true-without-write case also FAILS when the seed is reverted to `q{}` |

Flapping: a fixed-seed ±0.05 random walk around every threshold for 30 simulated
minutes yields ≤ 1 transition per dwell period (§9 of the original spec).
Property-style: monotonic — raising `sleep` never lowers the reported alertness rung.

## 10. Failure behavior
- No required input for > its limit (D5) ⇒ `Unknown` immediately, reason
  `SensorUnavailable`; `AlertnessProbabilities::valid == false` counts the same.
- Quality: `alertness.input_quality == Insufficient` or **both** sources `Insufficient`
  ⇒ `Unknown`, reason `LowSignalQuality`. A single `SourceQuality` source marked
  `Insufficient` does **not** gate on its own — a dead PPG must not condemn the IMU
  stream (00 §6); the alertness side decides through
  `AlertnessProbabilities.input_quality`/`valid` (D5).
- Valid inputs but no sustained evidence ⇒ `Unknown`, reason `InsufficientEvidence`
  (D8/E3) — never a fabricated `Awake`.
- Non-finite or out-of-range probability ⇒ treated as invalid (D13); never clamped
  into a plausible claim.
- A rejected `publish()` (`Err::Full` from F17) is the pump's problem (D11); engine
  state never depends on it.
- No allocation, no blocking, no platform calls in `step()`; durations saturate at
  `kMaxDurationFieldMs`. Sensor-fault *reporting* belongs to F23/F26, out of scope.

## 11. Budget
`StateEngine` ≈ 0.3 KB (config ≈ 15 fields, 2 smoothers, 4 counters, state fields),
`LatestValue<StateSnapshot>` ≈ 40 B (snapshot is 28 B), `StateOutputs` ≈ 0.1 KB on the
caller's stack, `StatePump` statics (engine + held `StateInputs` ≈ 60 B + 5 sequence
cursors + 2 quality stamps, D15) ≈ 0.2 KB ⇒ static **RAM < 1 KB**
(estimate by field count; measure the stack high-water mark at Validate). Task stack:
`kStateTaskStackBytes` = 3 KB, measured via `uxTaskGetStackHighWaterMark` at Validate
and trimmed if >1 KB spare. CPU: ~10 floating-point ops + ~20 comparisons per 1 Hz
step — negligible against a 1 s period.

## 12. Out of scope
- Sleep episode bookkeeping (F08).
- Alert levels (F10).

## 13. Open questions / assumptions
- ~~Final transition table review with the developer at end of Design stage~~ — **done
  2026-10-11**: the §7 normative table and D8/E3 were approved as written; this stage
  (PLAN) follows, then Implement + Test.
- F06 publishes `AlertnessProbabilities` at least every 30 s and, between epochs,
  **holds the last answer** — it does **not** flip `valid = false`. *(Corrected
  2026-10-11 against F06 as landed: the previous wording, "sets `valid = false` between
  epochs", was false. `valid` means "the last epoch produced a usable answer";
  staleness between epochs is judged from `t_ms` by `kAlertnessStaleMs` — exactly D5's
  per-producer `t_ms` handling, which sizes the constant.)*
- `[ASSUMED]` probabilities are per-class in `[0,1]` and roughly normalized; the rules
  only rely on `[0,1]` and on `drowsy+sleep` saturating at `kMaxProbability` (D7/D13).
- `[ASSUMED]` F04 does not itself publish `Event{DeviceNotWorn}` and F08 does not
  publish `DrowsinessEvent` — F07 claims both types (D12); both were unassigned in the
  specs when this design was written.
- W6 P4 resolves mode/power sourcing: optional `PumpPorts::mode_provider`
  (`IModeProvider`, CR-027), startup-bound by composition, mirrors published
  values; absent/unpublished = `Day`/`Normal`. F07 behaviour is unchanged (D10).
- `[ASSUMED]` `Event.payload[0..1]` for F07's two types may be ignored by generic
  decoders (F17/F28) — they were zero-reserved in the contract.
- **Resolved (design review, 2026-10-11):** ambiguous-but-stable evidence stays
  `Unknown` (`InsufficientEvidence`) instead of falling back to `Awake` (D8/E3) —
  approved as written.
- **CR-008: accepted + applied 2026-10-11** (commit `697da91`) — bits 12–14 exist in
  `bw_core/include/bw/state.h`; implement D9 with the real codes, no fallback.
- **CR-010: accepted + applied 2026-10-11** — `AlertnessProbabilities` gained a
  trailing `ReasonMask reasons` (evidence F06 saw); the merge policy into
  `StateSnapshot.reasons` is **D14** (pass-through OR for ladder states only).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Transition table and reason codes documented and covered by tests.
- CR-008 accepted + applied (2026-10-11, `697da91`): D9 implemented with the real
  codes — no gate-bits-only fallback.
- CR-010 merge policy (D14) implemented with its tests: pass-through OR on ladder
  states, never on gate states.

## 15. Implementation plan (PLAN stage 2026-10-11; approved design — not a redesign)

No production code lands in the PLAN stage. This section is the order of work for the
IMPLEMENT+TEST session; every module, test file, constant and rule it references is
defined in §6–§9 above.

Registries: `detector_id = 7` (`kStateFeatureId`, already allocated to F07); event
types `DrowsinessEvent` + `DeviceNotWorn` (D12) — all pre-existing values, **no new
enum values, no CR needed**. `bw_state` depends on `bw_core` only; no cross-feature
includes.

### The 1 Hz pump (inside `wire_state.cpp` — wiring may hold platform calls, no logic)
Per tick, `vTaskDelayUntil` with `kStateTaskPeriodMs` (no `delay()`):
1. `t_ms = ctx.clock->nowMs()` — the pump, never the engine, reads the clock (D4).
   `[ASSUMED]` **resolved at implement time:** `ctx.clock` (assigned by `f01::wire`,
   which runs first in the fixed call order) is read directly; no `millis()` adapter was
   needed. A null clock ⇒ the tick is skipped honestly, never stepped at `t = 0`.
2. **Sequence-gated pulls** (F03's pattern): each axis is `get()` once per *new*
   `sequence()`, into a held `StateInputs` (file-static storage, no heap). A producer
   that stops publishing therefore AGES out via its own `t_ms` (D5) — a stale mailbox is
   never re-read as fresh. Never-received ⇒ the contract default (`t_ms = 0`,
   `valid = false`, `Uncertain`/`Unknown`) with `SourceQuality` forced to `Insufficient`;
   at boot the gates then produce `Unknown` + `SensorUnavailable` regardless (§9
   `test_gates`).
3. `engine.step(inputs)` → publish `outputs.snapshot` to the bound `state_snapshot`
   mailbox.
4. `outputs.event_valid` → `IEventSink::publish(outputs.event)` through
   `attachEventSink()`'s pointer, **null until F17 lands** — a null or rejected publish
   is dropped and counted (`droppedEvents()`) by the pump, never by the engine (D11).

**P4 mailbox seam (wiring-owned, `f10::attachOutputs` precedent):** the three W1 fields
are *not* compile-time dependencies of `wire_state.cpp` — they are bound by one extra
integrator line next to `f07::wire(ctx)`:
`f07::attachMailboxes(&ctx.wear_estimate, &ctx.alertness_probabilities, &ctx.state_snapshot);`
Until that call the ports are nullptr = never-received (the tested gate path: `Unknown` +
`SensorUnavailable`, no snapshot published, no fabricated `Awake`).

**P4b quality-provider seam (Wave 4, D16):** `PumpPorts` gains one optional port —
`bool (*quality_provider)(bw::SourceQuality& out) = nullptr` (function pointer: no
heap, no `std::function`, no include of another feature's header) — and
`wire_state.*` gains the attach seam, bound ONCE by the composition root beside
`attachMailboxes`:

Invariants (regression-critical): **absent** ⇒ the feature-mailbox derivation runs
byte-for-byte as today (missing ⇒ `Insufficient`, A-03 path untouched, the existing
67 tests unaffected); **bound** ⇒ each `true` return drives both quality channels
(feature receipts can never refresh the provider's stamps — full substitution),
and `false`/silence
ages back to `Insufficient` via `kFeatureQualityStaleMs` (no new constant, no
retune, no new registry value, no CR — `PumpPorts`/`wire_state.*` are F07-owned,
§2/D15). Budget (measured, FIX loop 1): +4 B in `PumpPorts` (ESP32 function
pointer) + **16 B** in `StatePump` members — the 2 B `SourceQuality` channel +
6 B alignment padding to the 8-byte-aligned `TimeMs` + the 8 B receipt stamp
(host `sizeof` probe: member delta 16 B, port delta = pointer width 8 B host /
4 B ESP32; xtensa aligns `uint64_t` to 8 identically, so the total P4b cost is
20 B on ESP32, not the 10 B first estimated here) — one indirect call + one
`SourceQuality` copy per 1 Hz tick; §11 otherwise unchanged. Tests: the seven
`test_pump.cpp` provider cases added to §9 (the original four, plus the FIX
loop-1 recovery / mid-rung / true-without-write cases), registered in
`test_bw_state/main.cpp`; H10 revert check = disable the provider block in
`StatePump::tick()` ⇒ all seven must FAIL (provider value must reach the
quality gate / staleness path); reverting the defensive out-param seed to
`q{}` ⇒ the true-without-write case must FAIL (zero-initialized `Good`, never
fabricated healthy data).

Task: core 1, priority **3** (`kStateTaskPriority` — ≤ F03's intelligence pump, so the
consumer never preempts the producers it reads; Alert/Input tasks run strictly higher),
stack `kStateTaskStackBytes`, task function declared in `wire_state.h` per the
wiring README. Task health (last run, max lateness) is exposed via `lastRunMs()` /
`maxLatenessMs()`; reporting to F23 attaches when F23 exists.

### Order of work (small commits, message prefix `F07:`; tests only grow)
1. `state_config.h` — §8 constants + `StateEngineConfig` (every threshold a field
   defaulting to the constant).
2. `state_types.h` — `StateInputs`/`StateOutputs` + `static_assert`s (trivially
   copyable, 02 §1).
3. `smoother.h` (EMA, seeds on first sample) + `test_smoother.cpp` → green.
4. `duration_counter.h` (resets never pauses, saturating) + `test_duration_counter.cpp`
   → green.
5. Gates G1–G3 in `state_engine.cpp` + `test_gates.cpp` → green (rows G1, G2, G3a,
   G3b + flapping-wear case).
6. Ladder E1–E4 + `test_ladder.cpp` → green (table rows, boundary values, dip-resets,
   30-min random-walk ≤ 1 transition per dwell, monotonicity property).
7. Snapshot construction: confidence + reasons with the **real CR-008 codes** (D9) and
   the **D14 pass-through** + `test_snapshot.cpp` → green.
8. Transition records (D12) + `test_events.cpp` → green (incl. merged-mask assertion).
9. `wire_state.{h,cpp}` + `StateTask`; `pio run -e esp32dev` clean, `pio test -e native`
   still green.
10. `clang-format` on owned files; spec status log + §14 DoD self-check

---
