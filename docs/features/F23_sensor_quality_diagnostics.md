# F23 — Sensor quality & diagnostics

> Track **A** · Milestone **M2 (basic) → M5** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Continuously judge data quality per source (IMU, PPG) and system health, so the system can say "I don't have reliable data". Produces `SourceQuality` and `HealthSnapshot`.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `MotionFeatures`, `PpgFeatures` (their own quality hints)
- `SensorStatus` of drivers
- `BatteryReading`
- `TaskHealth` reports, heap stats (via a `ISystemStats` provider from wiring)
- BLE connection state provider

**Outputs (produced contracts)**
- `SourceQuality` (1 Hz)
- `HealthSnapshot`
- `IFaultReporter` calls for diagnosed faults (poor contact, stuck sensor, low battery)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Decide alertness/alerts.
- Hide bad data by smoothing it.
- Reset hardware (F26).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Quality model**
- Simplest: Boolean OK/not OK.
- Professional/scalable: Three levels (Good/Degraded/Insufficient) with per-source reasons and hysteresis.
- Trade-offs: More states to consume; enables graceful degradation and honest confidence.
- **Choice:** Three levels, reasons as fault codes.

**D2. Stuck-sensor detection**
- Simplest: None.
- Professional/scalable: Variance/identical-value run-length checks for IMU; saturation/flat-line checks for PPG.
- Trade-offs: False positives on a perfectly still desk device (IMU noise never exactly zero on healthy MPU6050).
- **Choice:** Run-length identical raw counts ≥ `kStuckSamples` ⇒ fault (boundary **inclusive**: a run of exactly `kStuckSamples` faults — 250 identical samples fault, 249 do not; pinned by `test_imu_raw_run_250_faults_249_does_not`. fault.h phrases the rule loosely as "> kStuckSamples"; the pinned, tested boundary is the code's ≥).

## 7. Module structure
- `ImuQualityAssessor`
- `PpgQualityAssessor`
- `SystemHealthMonitor`
- `QualityHysteresis`
- `DiagConfig`

**M5 additions (planned §16, W6):**
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMaxTrackedTasks` | 19 | `DERIVED` | W6 P4: original 12 keys plus independent wear/wake/sleep/cataplexy/log/baseline/optional research tasks; one writer per slot |
| `kDiagRawQueueImuSamples` | 64 | `DERIVED` | 1 s at F01's 50 Hz + 28 % jitter headroom, mirroring F02's `imu_samples` queue sizing rule (app.h) — a slightly-late DiagTask tick must not fabricate a data gap |
| `kDiagRawQueuePpgSamples` | 128 | `DERIVED` | same rule at F01's 100 Hz (app.h `ppg_samples` precedent) |
| `kMinTaskStackFreeBytes` | 512 | `EXPERIMENTAL` | per-task stack watermark below this ⇒ `LowStack` (CR-032): 512 B ≈ FreeRTOS TCB + ISR-nesting worst case on the smallest task stack (2048 B); tune with device high-water marks at DoD |
| `kDiagSelfTaskSlot` | 11 | `DERIVED` | original DiagTask key retained; newly adopted tasks append after it |
| `kLowStackFaultCode` | 26 | `DERIVED` | mirror of `bw/fault.h FaultCode::LowStack` (CR-032, already on this branch) — the rate-limit/sticky tables cover the contiguous 11–18 run, so the composer needs the append-only value spelled out; `static_assert` pins it to the enum |
| `kStackWatermarkUnitBytes` | 1 | `HW` | `uxTaskGetStackHighWaterMark` unit on this platform: the shipped IDF 4.4 header states verbatim "in bytes not words, unlike vanilla FreeRTOS" ⇒ the plan's `[ASSUMED]` ×4 is disproven and the conversion factor is 1 (single conversion point `stackWatermarkBytes()`, test-covered) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Fixtures: healthy, flat-line, saturated, dropouts, gap-ridden streams.
- Quality hysteresis; battery thresholds; overload detection.

## 10. Failure behavior
- Diagnostics itself failing ⇒ default `Insufficient` (fail-safe).

## 11. Budget
RAM < 1 KB (M2 basic).
M5/full slice: static RAM ≤ 12 KB `[DERIVED]` — M2 630 B + registry (12 ×
`LatestValue<TaskHealth>`) + snapshot + the raw SPSC queues and drain windows
(§16.5). The raw evidence windows are the memory price of on-device
stuck/saturation diagnosis; 12 KB ≈ 3.75 % of the 320 KB app RAM budget
(01 §3.4), inside the 150 KB global app allowance. Measured + asserted in
`test_diag_sizes_within_budget`.

## 12. Out of scope
- Hardware recovery actions (F26).

## 13. Open questions / assumptions
- Battery monitoring is optional: `valid=false` when no divider is configured.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Each fault code has a test and a documented cause/remedy.

## 15. Implementation plan — M2 basic (W2, stage PLAN, 2026-10-12)

Scope of this plan: **BASIC M2 only** — `SourceQuality` + `HealthSnapshot` producers and the
`IFaultReporter` calls for diagnosed faults, with `ISystemStats` supplied by wiring. M5/full-F23
items (BLE-dependent diagnostics, raw-feed hookup, deep overload analysis) are explicitly deferred
in §15.9. No production code exists yet at this stage; §15 is the contract for the IMPLEMENT session.

### 15.1 Module breakdown (`firmware/lib/bw_diag/`)
| File | Contains | Purity |
|---|---|---|
| `include/bw/diag/diag_config.h` | `DiagConfig` struct: all §8 thresholds as fields with the tagged defaults | pure header |
| `include/bw/diag/quality_hysteresis.h` + `src/quality_hysteresis.cpp` | `QualityHysteresis` — per-source enter/exit counters (D7) | pure |
| `include/bw/diag/imu_quality_assessor.h` + `src/imu_quality_assessor.cpp` | `ImuQualityAssessor::step()` — status/feature rules + identical-run checks (raw: `kStuckSamples`; feature frames: `kStuckFeatureWindows`) → `{DataQuality, optional FaultEdge}` | pure |
| `include/bw/diag/ppg_quality_assessor.h` + `src/ppg_quality_assessor.cpp` | `PpgQualityAssessor::step()` + `evaluatePpgRawWindow(samples, n)` (saturation / flat-line / contact on one raw window) → `{DataQuality, FaultEdge}` | pure |
| `include/bw/diag/system_health_monitor.h` + `src/system_health_monitor.cpp` | battery / heap / lateness / BLE → `HealthSnapshot` fields + `LowBattery`/`CriticalBattery`/`LowHeap`/`TaskOverrun` edges | pure |
| `include/bw/diag/diagnostics.h` + `src/diagnostics.cpp` | `Diagnostics::step(const DiagInput&) → DiagOutput` — composes the modules, applies hysteresis, fills `SourceQuality` + `HealthSnapshot`, accumulates `fault_bits`, rate-limits report edges (D9) | pure |
| `firmware/test/bw_diag/test_bw_diag/**` | Unity suite + fixtures (§15.5) | host |

Feature-local value types (NOT contracts; live in `bw/diag/diagnostics.h`):
`SystemStats` (the `ISystemStats` of §3 — see D4), `DiagInput`, `DiagOutput`, `FaultEdge`.
Sketch:
```cpp
struct SystemStats { bool valid; uint32_t min_free_heap; uint16_t worst_task_lateness_ms; bool ble_connected; };
struct DiagInput {
    TimeMs now_ms;
    bool has_imu_status; SensorStatus imu_status;     // F01 via wiring (D4)
    bool has_ppg_status;  SensorStatus ppg_status;
    bool has_motion; MotionFeatures motion;           // age = now_ms - t_ms computed inside
    bool has_ppg;   PpgFeatures ppg;
    BatteryReading battery;                           // valid=false ⇒ unknown, no fault
    SystemStats stats;                                // valid=false ⇒ heap/lateness unknown, no fault
    const ImuSample* imu_raw; size_t imu_raw_n;       // nullptr at M2 device (D6)
    const PpgSample* ppg_raw; size_t ppg_raw_n;
};
struct DiagOutput { SourceQuality quality; HealthSnapshot health;
                    FaultEdge edges[kMaxFaultEdgesPerTick]; uint8_t edge_count; };
```

### 15.2 Design decisions (D3–D9; D1/D2 in §6)

**D6. Diagnosis granularity at M2 (instantiation of D2)**
- Simplest: raw-sample checks only — never fires on device at M2 (no legal raw consumer: `ctx.imu_samples`/`ctx.ppg_samples` are single-consumer F02; F28's tee is capture-gated `nullptr`) ⇒ dead code. Professional/scalable: feature/status-level rules only — cannot satisfy DoD tests for `PpgSaturated`/raw `ImuStuckSensor` (not derivable from `PpgFeatures`).
- **Choice:** hybrid. (a) Feature/status-level rules are the production path at M2: identical 1 Hz feature-frame runs (`kStuckFeatureWindows` — deterministic pipeline ⇒ bit-identical frames mean bit-identical raw input), constant `ir_dc_level` + low perfusion ⇒ flat-line, low perfusion ⇒ poor contact; status/staleness ⇒ `Insufficient` (fail-safe). (b) Raw-window functions (`kStuckSamples`, `kPpgFlatlineRawSamples`, `kSaturatedWindowFraction`) are implemented and host-tested now; their device feed is absent at M2 (`imu_raw`/`ppg_raw` = `nullptr` ⇒ those checks report "not evaluated", never a guess) and is **deferred to M5** (§15.9). F23 never double-reports F01's codes (`ImuAbsent`, `PpgAbsent`, `*ReadFailed`, `BatterySenseUnavailable`): it downgrades quality only.

**D7. Hysteresis shape**
- Simplest: symmetric enter/exit counts. Professional: fail-safe asymmetry.
- **Choice:** `Insufficient` enters **immediately** (status `Failed`/`Uninitialized`, missing/stale input — §10: never delay honesty); `Degraded` enters after `kDegradedEnterWindows`; any recovery to `Good` needs `kGoodRecoverWindows`. Trade-off: a single glitch drops the source instantly (safe direction) but recovery is slow (3 s) — accepted: gating errs toward "no reliable data".

**D8. `fault_bits` / `degraded_mode` semantics at M2**
- Problem: `HealthSnapshot.fault_bits` is produced by F23 but `IFaultReporter` is write-only — no read path from F26's registry exists (F00 §16 observation 4; the CR belongs to F23/F26, **deferred** to M5, §15.9).
- **Choice:** `fault_bits` bit *N* set ⇒ `static_cast<uint8_t>(FaultCode) == N` (values 11–18 < 32) diagnosed **by F23 this boot**, sticky, never cleared; other features' faults are absent until the read-path CR lands (documented, not fabricated). `degraded_mode = (imu != Good) || (ppg != Good)` `[ASSUMED]` working definition — capability reduced by data quality; F26 refines when its degradation table lands.

**D9. Fault reporting policy**
- Edge-triggered: an edge is emitted when a diagnosis *transitions* (or would re-transition within `kFaultRepeatMs` — suppressed, so a flapping condition reports ≤ 1/min). Persistent conditions do not re-report; `fault_bits` keeps them visible.
- Severity (F00 deliberately assigns none — mapping here is F23's caller-side choice): `Error` = source unusable ⇒ `ImuStuckSensor`, `PpgFlatline`, `PpgSaturated`, `CriticalBattery`; `Warning` = still functioning ⇒ `PpgPoorContact`, `TaskOverrun`, `LowHeap`, `LowBattery` (matches fault.h's own examples for `PpgPoorContact`/`LowBattery`). Battery/heap invalid or BLE absent ⇒ **no** report (unknown ≠ fault).
- Boundary semantics pinned by fault.h comments: report `LowBattery` only when `percent < kLowBatteryPercent`, `CriticalBattery` only when `percent < kCriticalBatteryPercent`, `TaskOverrun` only when `lateness > kMaxTaskLatenessMs`.

### 15.3 Fault-code table (DoD §14: cause → effect → severity → remedy)
All eight codes **already exist** in `bw/fault.h` (values 11–18, pinned by `test_fault_codes.cpp`) — no enum change, no CR (§15.8).

**Raw-window rules (present-window evidence, I1).** When a raw window IS present it is current, direct evidence and overrides the feature-staleness fail-safe in defined ways:
- PPG: a tick without fresh features reports the raw-window evaluation itself (a healthy window ⇒ `Good` — the **raw-Good override**); `PpgFlatline`/`PpgSaturated` in the window force `Insufficient` on any tick; the driver hint stays a floor (`Degraded` status ⇒ at least `Degraded`).
- IMU: quality still cannot leave `Insufficient` without fresh features (a raw run cannot vouch for the 1 Hz feature stream), but a latched raw run emits `ImuStuckSensor` on the tick the window is evaluated — raw evidence beats feature staleness.
- An absent window is "not evaluated" (D6) and **ends any open raw episode**: latch + run reset, so a stale latch never pins a source and the next window must earn its own run from scratch.

### 15.5 Test plan (suite `bw_diag`, `firmware/test/bw_diag/test_bw_diag/`)
New suite, Unity, deterministic (FakeClock-style tick times passed in `now_ms`); 0 tests removed/loosened anywhere.
1. `test_quality_hysteresis.cpp` — enter after `kDegradedEnterWindows`; single bad window no change; recovery after `kGoodRecoverWindows`; immediate `Insufficient`; `Insufficient → Good` needs full recovery count; noise flapping settles at `Degraded`.
2. `test_imu_quality_assessor.cpp` — healthy → `Good`; `Failed`/`Uninitialized`/no status → `Insufficient` with **no** F23 edge; no/stale features → `Insufficient`; identical frames ×5 → `ImuStuckSensor` Error + `Insufficient` (edge once, then silent); ×4 → no fault; raw ×250 → fault, ×249 → none; recovery; absent raw window ends the raw episode (latch + run reset, next window earns its own run; re-edges as a new episode); stale features + stuck raw window edges on the stale path (audit L4); `TimeMs` > 32-bit gap; first-call empty state.
3. `test_ppg_quality_assessor.cpp` — healthy → `Good`; **bench case** (PPG absent: `Failed` status, no features) → `Insufficient`, no F23 edge; status Ok + no features → `Insufficient` (fail-safe); PI 0.001 → `PpgPoorContact` Warning, PI 0.003 → none; identical DC ×5 + PI≈0 → `PpgFlatline` Error; raw saturated ≥ 2 % → `PpgSaturated`, 1.9 % → none; raw flat-line ×250 → `PpgFlatline`; raw `n=0` while Ok → `Insufficient`; F02 hint worst-of; `Degraded` status worst-of (fresh + raw-override paths, §15.4); stale features + raw window: saturated beats staleness, healthy raw reports the raw-Good override, absent window ⇒ fail-safe (audit L4); `motion_corrupted` → ≥ `Degraded`.
4. `test_system_health_monitor.cpp` — invalid battery (bench) → flag only; 25 % none; 19.9 → `LowBattery`; 20.0 → none; 7.9 → `CriticalBattery` + `LowBattery`; 8.0 → `LowBattery` only; heap 16383 → `LowHeap`, 16384 → none; lateness 51 → `TaskOverrun`, 50 → none; invalid stats → no faults; `fault_bits` accumulation; `degraded_mode`.
5. `test_diagnostics.cpp` — §10 fail-safe (empty input → `Insufficient`/`Insufficient`, `battery_valid=false`, `degraded_mode=true`); composition + `kMaxFaultEdgesPerTick`; rate limit (re-edge < `kFaultRepeatMs` suppressed, ≥ emitted); sticky `fault_bits` across ticks; out-of-range fault code (≥ `kFaultBitsWidth`) is a defined no-op — no shift UB, no bit, no edge (I2); `health.t_ms == now_ms`; end-to-end staleness/hysteresis timeline.
6. `test_no_heap.cpp` — global `new` counter: zero allocations across `step()` loops (pattern exists in `bw_ml`).

DoD traceability: each of the eight codes in §15.3 has ≥ 1 asserting test above + a documented cause/remedy.

### 15.6 Delivery shape of `wire_diag.*`
`wire_diag.h`: namespace `f23` · `void wire(app::AppContext&)` (services slot of the README call order, idempotent) · `void diagTask(void*)` · the four task constants (D3) · accessors (D5). `wire_diag.cpp`: static `Diagnostics` + static mailboxes; `gatherStats()`/`gatherInput()` bindings guarded `#if defined(ARDUINO)` (heap, `f01::*`); pure parts unguarded (README §3: compiles for both envs); report edges via `ctx.faults` with the **nullptr-sink fallback** (F01 D7 pattern: drop, never assume a sink). No `Serial`, no logic beyond gathering + one `step()` call per tick.

### 15.7 Budgets
- Static RAM (lib + wiring statics): rate-limit table ≈ 80 B, assessor state ≈ 120 B, `DiagConfig` ≈ 48 B, two output mailboxes ≈ 64 B ⇒ **≤ 512 B, budget < 1 KB (§11)** — verified with `sizeof` assertions in tests.
  *Measured in IMPLEMENT (host x86-64 `sizeof`, tests assert the bounds): `Diagnostics` 528 B (incl. 2×52 B hysteresis + 144 B IMU assessor + 96 B PPG assessor + 56 B health monitor + 72 B rate-limit table + 48 B config copy), `DiagConfig` 48 B, `DiagInput` 168 B ⇒ composer+config 576 B + wiring mailboxes ≈ 54 B ⇒ **≈ 630 B static, within the < 1 KB §11 budget** (over the ≤ 512 B plan estimate because each module carries its own `DiagConfig` copy; accepted for simplicity — no lifetime coupling to a shared config object). esp32dev delta confirms: +528 B RAM vs the pre-F23 build.*
- Task stack: 2048 B (D3) — measure `uxTaskGetStackHighWaterMark` at DoD.
- CPU: one `step()`/s, O(window) ≈ µs — no measurable share; re-measure at DoD if raw feed (M5) lands.

### 15.9 Explicitly deferred (out of BASIC M2, W2)
> **Resolved 2026-10-18 (W6 PLAN):** every item below is decided (do/defer+reason) in §16.0.
- BLE connection-state input (F19 is W4; full F23 is W6) → `ble_connected=false` until then.
- Raw-sample feed on device for stuck/saturation diagnosis (D6b) — needs a legal raw tap; M5. **The M5 feed is bound by the §15.4 raw-window rules**: windows are evaluated whenever present — including ticks with stale features (PPG raw-Good override; IMU stale-path `ImuStuckSensor` edge); an absent window ends the open raw episode (latch + run reset), so a capture-gated feed can never pin a source after its capture ends.
- Per-task `TaskHealth` registry per runtime.md §3.2 (mechanism does not exist yet) → M2 uses only existing stats (D4).
- Deep overload analysis (per-task stack watermarks, core-level analysis) — M5/full F23.
- `fault_bits` full-registry read path (CR, §15.8) — M5/W6 with F26.
- Hardware recovery actions (§12, F26).

---

## 16. Implementation plan — M5 / full slice (W6, stage PLAN, 2026-10-18)

Scope: the §15.9 deferred list, each item decided. No production code at this stage.
M2 modules are touched only where the new slice genuinely extends them (§16.2).

### 16.0 §15.9 resolution table

### 16.1 Module changes (`firmware/lib/bw_diag/`)

| File | Change | Type |
|---|---|---|
| `diag_types.h` | `DiagInput` grows: `bool has_reliability; ReliabilityState reliability{}` (CR-030 value type) and `const bw::TaskHealth* tasks; uint8_t task_count` (pointer+count — no copy, same pattern as the raw windows) | extend |
| `diag_config.h` | the five §8 M5 constants + `kLowStackFaultCode = 26` mirror once CR-032 lands | extend |
| `system_health_monitor.h/.cpp` | `step(battery, stats, tasks, task_count)`: worst lateness = max(legacy `stats.worst_task_lateness_ms`, max over **valid** slots — invalid slots excluded, unknown ≠ 0); new `LowStack` latch (any valid slot with `stack_free_min_bytes < kMinTaskStackFreeBytes`, one edge per episode, re-edges after full recovery — same latch shape as `LowHeap`) | extend |
| `diagnostics.h/.cpp` | D11 merge: `health.fault_bits = own_sticky_bits_ | (has_reliability ? reliability.fault_bits : 0)`; `degraded_mode = quality_based || (has_reliability && (reliability.degraded != 0 || reliability.safe_mode))` | extend |
| `task_registry.h/.cpp` (new) | pure-logic free helpers over a `bw::TaskHealth` snapshot array: `worstLatenessMs()`, `worstStackFreeBytes()`, `slotBelowStackFloor()` | new |
| `firmware/src/app/wiring/wire_diag.h/.cpp` | ① static `bw::LatestValue<bw::TaskHealth> s_task_slots[kMaxTrackedTasks]` + `noteTaskCycle(TaskSlot, TimeMs, lateness, stack_free)` (single-writer-per-slot read-modify-write: get→`noteCycle`→set; reader sees whole values via the mailbox critical section) + `bool latestTaskHealth(TaskSlot, bw::TaskHealth&)` accessor; ② raw tee: static `SpscQueue<ImuSample, kDiagRawQueueImuSamples>` + `SpscQueue<PpgSample, kDiagRawQueuePpgSamples>` + producer hooks `onImuSample()/onPpgSample()` + `attachReliabilityProvider(bw::IReliabilityStateProvider*)` attach seam (wire_ble `attachProviders` precedent); ③ `gatherInput()` drains queues into static window arrays, passes the existing `imu_raw/ppg_raw` fields; BLE: `f19::linkProvider()` → `stats.ble_connected` (ARDUINO-guarded) | extend |

### 16.2 Design decisions (D10–D15)

**D10. Where provider data enters: value copy at wiring, not provider pointers in pure logic.**
- Simplest: pass `IReliabilityStateProvider*`/`IBleLinkProvider*` into `Diagnostics::step()`. Professional: wiring calls `latest()` once per tick and copies the value into `DiagInput` (D4 precedent).
- Trade-offs: pointers would make every host test build a fake provider class; a value copy keeps `Diagnostics` a plain value-logic composer and keeps provider thread-safety where it already lives (LatestValue-backed providers, link_state_tracker.h:86).
- **Choice:** value copy. `ReliabilityState` is a `bw_core` value type, so putting it in `DiagInput` violates no layering rule.

**D11. `fault_bits` merge = sticky OR; honest fallback preserved.**
- Simplest: provider present ⇒ replace `fault_bits` with the registry's. Professional: union.
- Trade-offs: replace drops F23's own view the moment F26 restarts and its in-memory snapshot is gone; union never fabricates (both sides are diagnosed-this-boot evidence, sticky, same D8 bit mapping) and is idempotent (F23's codes reach the registry through `ctx.faults`, so overlap ORs harmlessly).
- **Choice:** `own | registry`, and **own-only when the provider is absent or returns false** (F26 wired in the same wave but its provider legitimately reports "no snapshot yet" until then — the M2 behavior is the fallback, never a fabricated zero or a fabricated foreign bit). `degraded_mode` likewise: the D8 quality definition stands alone when no snapshot exists; when one exists, `degraded != 0 || safe_mode` adds to it (CR-029 semantics; F26 refines further in its own feature).

**D12. BLE input: report F19's last link state as-is, no staleness gate.**
- Simplest: always false until someone wires BLE (M2). Professional: read `f19::linkProvider()`.
- Trade-off considered: gate `connected` on snapshot age (`BleLinkState.t_ms`)? **Rejected** — CR-018 snapshots are produced at event time (link_state_tracker.cpp:33), so a quiet-but-connected phone legitimately has an old `t_ms`; a staleness gate would fabricate disconnects. `ble_connected` feeds only the `HealthSnapshot` field — no quality effect and no F23 fault code (`BleDown` is F26's, surfaced to us through D11's merge). Null provider (host, bench, pre-wire) ⇒ false, honest.

**D14. Deep overload = per-task watermarks + one `LowStack` code; core-level split is derived, not stored.**
- Problem: stack exhaustion is invisible today; `HealthSnapshot` is pinned (32/8) and has no per-task or per-core field.
- Simplest: only raise the worst lateness (M2). Professional: per-task stack watermark diagnosis + per-core aggregates.
- Trade-offs: per-core stored aggregates would need a contract change nobody consumes; the per-task slot already names its task, and each task's core is a static table fact (runtime.md §3.2), so per-core worst = max over that core's slots — derivable anywhere, stored nowhere. `LowStack` needs a code → **CR-032** (§16.8); without it the watermarks would still be readable via `f23::latestTaskHealth` but undiagnosable.
- **Choice:** per-task collection + single latched `LowStack` Warning edge (any slot below `kMinTaskStackFreeBytes`; per-task identity lives in the registry accessor, not in the fault code). Watermarks stored as reported — the wiring converts FreeRTOS units to bytes at the call site (`[ASSUMED]` Arduino-ESP32 core 2.x = IDF 4.4 returns **words**; verify at IMPLEMENT, one conversion constant, flagged in §16.6).

### 16.3 Fault-code extension (DoD §14, cause → effect → severity → remedy)

**P3 review refinement (D16, 2026-10-18).** Problem: device raw windows are bounded to 64 IMU/128 PPG samples, producer and consumer run concurrently, and held feature mailboxes are not new evidence. Simplest: trust polling cadence and per-window checks; scalable: track evidence identity/continuity and bracket queue draining. Choice: feature-frame runs advance only on distinct increasing timestamps; absent/stale feature evidence breaks the run, and failed/missing driver status resets assessor continuity. Raw PPG IR runs span consecutive bounded windows (otherwise the 250-sample threshold was unreachable on device), reset on an absent/drop-hole window, and preserve saturation precedence. Newly crossed raw thresholds inside a window are not lost if a later sample breaks the run; a previously latched episode still recovers on a clean window. Drain helper checks drops before AND after popping; a deterministic scripted queue injects a producer drop during pop to test this interleaving. No thresholds changed, no heap, no new shared contract. Cross-task sensor status now comes from a SensorTask-published LatestValue copy (`noteSensorStatuses`), not mutable F01 fields. Reliability attachment pointer publication is atomic; lifetime remains the composition's responsibility. DiagTask creation failure reports existing CR-033 `TaskCreateFailed` at Error through a host-testable report seam. P4 must add the sensor-status producer call; raw tee producer calls remain as planned. These refinements supersede D4/§15.4 direct driver-status reads and D6's per-window-only PPG assumption.

All other codes unchanged (§15.3 table stands).

### 16.4 Test plan additions (suite `bw_diag`; 0 tests removed or loosened; M2 `test_diag_sizes_within_budget` expectations **updated** — see note)

1. `test_diagnostics.cpp` (+): fault_bits = own-only when `has_reliability=false` (fallback pinned); union with foreign registry bits (e.g. bit 19 `TaskStalled`); sticky union across ticks; provider vanishing after being present keeps own bits, drops foreign ones (honest "what the registry says now"); `degraded_mode` matrix — no provider + quality Degraded ⇒ true (M2 fallback preserved), provider `degraded=0/safe=false` ⇒ quality-only, `degraded!=0` ⇒ true, `safe_mode=true` ⇒ true; foreign bits never produce edges or enter F23's rate limiter (I2 rule re-pinned through the new path).
2. `test_system_health_monitor.cpp` (+): worst lateness = max over valid slots; invalid/absent slots excluded (not counted as 0); legacy `stats` field still floors the max (transition period); `LowStack` at 511 / silent at 512; multi-task low ⇒ single edge per episode; recovery ⇒ re-edge allowed; `TaskOverrun` 51/50 boundary re-pinned via the tasks-array path.
3. `test_task_registry.cpp` (new): `worstLatenessMs` / `worstStackFreeBytes` / `slotBelowStackFloor` over healthy/empty/all-invalid arrays; first-call empty state.
4. `test_wire_diag_compile.cpp` (+): `noteTaskCycle` round-trip (note → gather → `DiagInput.tasks` carries it, max-keeping per `TaskHealth::noteCycle`); raw ingest 64×Imu/128×Ppg → window present; **drop-hole** ⇒ window suppressed ⇒ raw episode ends (§15.4); queue-refused push counted, never crashes; `latestTaskHealth` false before first note; native shape: `ble_connected` false, `has_reliability` false (honest defaults).
5. `test_diagnostics.cpp` (+): cross-tick raw accumulation — five consecutive 50-sample windows with one identical run reach `kStuckSamples` ⇒ `ImuStuckSensor`; a drop-hole between windows restarts the run from scratch (reverting the D15 rule must fail this test).
6. `test_no_heap.cpp` (+): registry RMW loop + raw drain loops — zero allocations across 1000 iterations.
- Size expectations in `test_diag_sizes_within_budget` move to the M5 values (DiagInput ≈ 216 B etc.) — an asserted bound change with §16.7 justification, not a loosening.
- `[ASSUMED]` verify-at-IMPLEMENT note: `uxTaskGetStackHighWaterMark` unit — **verified BYTES, not words**: the shipped IDF 4.4 header states verbatim "in bytes not words, unlike vanilla FreeRTOS" (`framework-arduinoespressif32/tools/sdk/esp32/include/freertos/include/freertos/task.h`), so the plan's ×4 words conversion is CONTRADICTED; the wiring pins `kStackWatermarkUnitBytes = 1` `[HW]` as the single conversion constant, test-covered with the byte value (`test_wire_diag_stack_watermark_unit_is_bytes`).

### 16.5 Budgets (§11 update)

> **CR-032 — append `FaultCode::LowStack`**
> - Raised by: F23 / 2026-10-18 · Type: **additive**
> - Problem: M5 deep-overload analysis (runtime.md §3.2: tasks report stack high-water marks to F23) has no vocabulary for "task stack near exhaustion". Reusing `LowHeap` would conflate heap and stack (different cause, different remedy) — fabricating a symptom name.
> - Proposal: `bw/fault.h` enum append: `LowStack,  // F23 M5: TaskHealth.stack_free_min_bytes < kMinTaskStackFreeBytes`. Value 26 (after `BaselineCorrupt` = 25); pin-test count 26 → 27.
> - Alternatives: reuse `LowHeap` (rejected: dishonest conflation); no code, registry-only (rejected: undiagnosable — no `IFaultReporter` path, DoD §14 unmeetable).
> - Affected: F23 (producer), F26 (registry superset gains a bit), F00 pin test.
> - Compatibility: append-only (02 §6.6); `fault_bits` bit 26 representable (< 32).

1. P4: producer one-liners `f23::onImuSample/onPpgSample` in F01's SensorTask (`wire_hal.cpp`, F28 A1 pattern).
2. P4: `f23::attachReliabilityProvider(...)` bound to F26's provider once F26's wiring exposes it (or F26's wire calls it).
3. P4: `f23::noteTaskCycle(...)` one-liner per adopted task (11 wire files); until then slots stay invalid = excluded.
4. Approval: DiagTask stack 3072 B; §11 budget 1 KB → 12 KB; CR-032 filing.

---
