# F26 — Fault handling & reliability

> Track **E** · Milestone **M5** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Graceful degradation and recovery: fault registry, degradation policy (which features require which sources), watchdog supervision, brownout/boot-loop protection, crash log, and `reinit` scheduling for failed sensors.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `IFaultReporter.report()` from all features
- `SensorStatus`, `HealthSnapshot`
- Task heartbeat reports

**Outputs (produced contracts)**
- `FaultRegistry : IFaultReporter`
- `DegradedMode` flags (PPG lost, IMU lost, storage degraded, BLE down)
- Watchdog feeding; reset reason capture; `Event{SensorFault, Boot}`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Contain feature-specific logic beyond the degradation table.
- Feed the watchdog from a task that can stay alive while the pipeline is dead.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Watchdog design**
- Simplest: Feed the watchdog from one loop.
- Professional/scalable: Task watchdog + heartbeat aggregation: supervisor feeds the hardware WDT only when all critical tasks reported within their deadlines.
- Trade-offs: More bookkeeping but detects one dead task.
- **Choice:** Heartbeat aggregation.

**D2. Degradation table**
- Simplest: If/else in features.
- Professional/scalable: Central table: feature → required sources → fallback.
- Trade-offs: Central table needs owner review when features change.
- **Choice:** Central table (e.g. PPG failure ⇒ IMU-only estimation, degraded flag).

## 7. Module structure
- `FaultRegistry`
- `DegradationPolicy`
- `WatchdogSupervisor`
- `BootGuard` (boot-loop counter → safe mode)
- `CrashLog`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kTaskWatchdogTimeoutS` | 5 | `EXPERIMENTAL` | Longer than the slowest task period by 5× |
| `kMaxRebootsInWindow` | 3 in 10 min | `EXPERIMENTAL` | Enter safe mode (sensing + alerting only) after this |
| `kReinitBackoffMs` | 1000 → 30000 exponential | `EXPERIMENTAL` | Avoid hammering a dead bus |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Fault-injection matrix on host: each fault → expected degraded flags and events.
- Watchdog supervisor with fake heartbeats.
- Boot-loop logic with simulated reset counters.

## 10. Failure behavior
- The supervisor itself is the last line: if it stalls, hardware WDT resets.

## 11. Budget
RAM < 2 KB.

## 12. Out of scope
- Individual driver internals (F01).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Degradation table published and matches tests.

## 16. Plan details

### 16.1 Design decisions (D3–D14; D1/D2 in §6 unchanged, this table refines them)

**D3. FaultRegistry shape (refines D1's aggregation point)**
- Problem: `IFaultReporter.report()` is called from many tasks; F23/F12/F24 need
  to READ the aggregate; no read path exists today (`bw/fault.h:59`).
- Alternatives: (a) simplest — forward every report straight to F23's public
  `Diagnostics::consider()`; no local state, no new contract. (b) Registry keeps
  a per-code record `{count, first_ms, last_ms, max_severity}` + latched bits +
  transition edges for the degradation policy, exposes bits via a provider (CR-030).
- Trade-offs: (a) is small but makes a 1 Hz task the only aggregate, loses
  counts/severity history, and contradicts 01 §7 ("F26 aggregates"); every
  future consumer would depend on F23's internals. (b) costs ~600 B static and a
  critical section in `report()` (reports arrive from several tasks) but keeps
  aggregation in the owning feature and is host-testable as a pure core.
- **Choice:** (b). Pure `FaultRegistry` core (no platform calls) + wiring-level
  `portMUX` guard around `report()`. Out-of-range codes (≥ kMaxFaultCodes) are a
  defined no-op (F23 I2 precedent), never UB.

**D4. `fault_bits` semantics for CR-030 — align with F23 D8**
- bit *N* ⇔ `FaultCode` value *N*, set on first report this boot, **sticky,
  never cleared**; registry bits are a superset of F23's local bits (F23 already
  routes its own edges through `ctx.faults`, `wire_diag.cpp` runTick). No
  clearing in v1: no `FaultCleared` code exists (append-only) and per-code
  expiry timers buy nothing a `SensorStatus`/event read cannot; recovery stays
  visible via sensors and events, not via bits. `[ASSUMED]` — noted for G1.

**D5. Degradation inputs (refines D2's table mechanics)**
- `DegradationPolicy::step()` consumes three optional inputs, each with an
  honest "absent = unknown, never down" rule: fault codes (from the registry),
  `SensorStatus` for IMU/PPG (wiring-level `f01::` reads, the `wire_diag.cpp`
  precedent), and provider snapshots `IStorageHealthProvider` (CR-015) /
  `IBleLinkProvider` (CR-018) behind nullable pointers.
- The policy itself PRODUCES `StorageDegraded` (24-range code 23) and `BleDown`
  (24) back into the registry when provider conditions hold, closing the loop
  (those two codes' comments in `bw/fault.h` name F26 as the source).

**D6. `DegradedMode` contract — raise CR-029**
- Problem: spec §3 produces "DegradedMode flags" but the vocabulary has no
  `bw_core` home; consumers outside `bw_reliability` may not include feature
  headers (01 §1 r1).
- Alternatives: (a) no new type — consumers decode `fault_bits` themselves;
  (b) enum+mask in `bw_core` (CR-029). (a) re-scatters the if/else D2 rejects
  and couples consumers to symptom-code numbering; (b) is 1 enum + 1 typedef.
- **Choice:** (b) — see the CR-029 draft in §16.7. Flags: `PpgLost, ImuLost,
  StorageDegraded, BleDown, SafeMode` (append-only, `Count` sentinel).

**D7. Supervision coverage + tiers (answers backlog 33)**
- Two tiers, because a reset is a last resort, not a service outage response:
  - **Tier A (gates the hardware-WDT feed → stall ⇒ reset):** `SensorTask`,
    `ProcessingTask`, `IntelligenceTask`, `StateTask`, `AlertTask`, `InputTask`
    — the sensing→decision→alert path; if it is dead the device must reboot.
  - **Tier B (supervised, stall ⇒ report + degrade, no reset):** `DiagTask`,
    `UiTask`, `StorageTask`, `CommsTask`, `ServicePump` — covers all four
    backlog-33 tasks. Rationale: `DisplayFailed` must never block sensing
    (fault.h), BLE down must never stop local alerting (00 non-negotiable),
    storage/comms death degrades per the §16.4 table; only Tier A death
    justifies a full reset.
- Stall rule: `now - last_beat > kStallFactor × task_period`, and only after
  the task's first beat (before that it is "pending", never stalled); a boot
  grace window covers staggered task creation.

**D9. Watchdog mechanics (spec §5 rule)**
- The supervisor subscribes **only itself** to the ESP32 TWDT
  (`kTaskWatchdogTimeoutS`) and calls `esp_task_wdt_reset()` **only when every
  Tier A heartbeat is fresh**. No loop that can survive a dead pipeline ever
  feeds the WDT: if a Tier A task stalls, the feed is withheld → TWDT panic →
  reset; if the supervisor itself stalls, nobody feeds → reset (spec §10).
- Rejected: per-task TWDT subscriptions — every task must feed (invasive) and a
  task stuck inside a broken driver could still feed; aggregation gives policy
  control. Core-0 IDLE starvation also trips the TWDT (Arduino-ESP32 default),
  which is the correct verdict (a blocked core-0 trio is a real fault).

**D10. Boot-loop guard + safe-mode definition (§13 open question — proposal for G1)**

### 16.2 Module structure (`firmware/lib/bw_reliability/`)
```
include/bw/reliability/
  reliability_config.h   // constants of §16.3
  fault_registry.h       // pure registry (IFaultReporter impl core, bits, edges)
  degradation_policy.h   // table-driven policy, DegradedMode mask out
  watchdog_supervisor.h  // slot table, beat(), stall scan, feed decision
  boot_guard.h           // streak logic, safe-mode decision (pure)
  crash_record.h         // fixed record codec (serialize/parse, schema tag)
  reinit_scheduler.h     // backoff logic (D12)
src/                     // the above .cpp, all pure logic
firmware/src/app/wiring/wire_reliability.{h,cpp}   // platform shell + task
```
No feature includes any of this except at wiring level (L5), per 01 §1 r1.

### 16.4 Degradation table (DoD §14 — published; test-per-row in §16.8)
| # | Trigger (input) | Facility flag / code | Affected | Fallback behavior | Side effects |
|---|---|---|---|---|---|
| 1 | PPG `SensorStatus::Failed` or code PpgAbsent/PpgReadFailed/PpgFlatline/PpgSaturated/PpgPoorContact | `PpgLost` | F04 (PPG part), F06 PPG evidence, F09, F23 PPG quality | PPG-dependent estimates report `Insufficient`; IMU-only pipeline continues; consumers lower confidence per their specs | SensorFault edge, code latched in `fault_bits` |
| 2 | IMU `Failed` or ImuAbsent/ImuReadFailed/ImuStuckSensor | `ImuLost` | F02–F04, F06–F10 motion path | state falls to Unknown-capable path (F07's job); no confident drowsy/sleep inference; button alerting/ack keeps working | SensorFault edge |
| 3 | `IStorageHealthProvider`: corrupt_records > 0 or free < `kStorageDegradedFreePct` or counters growing | `StorageDegraded` + code 23 | F17/F18 logging, F20 sync, F28 capture | keep alert-class events; drop research windows first; report; sync retries later | SensorFault edge (once per transition) |
| 4 | `IBleLinkProvider` event-time state reports disconnected continuously for ≥ `kBleDownConfirmMs` after an observed connection; absent/future-dated input breaks the timer | `BleDown` + code 24 | F19/F20/F21 | continue fully local; events buffered in F18; sync on reconnect. **Alerting never waits on BLE** (00 rule) | SensorFault edge only on confirmed loss-after-connection, never on never-connected idle state |
| 5 | Tier A heartbeat stall (D7) | code `TaskStalled` | whole pipeline | supervisor withholds TWDT feed → HW reset; code latched + crash record written best-effort | next boot: `WatchdogReset` + streak++ (row 7/8) |
| 6 | Tier B heartbeat stall (Diag/Ui/Storage/Comms/ServicePump) | mapped flag (Storage→row 3 flag, Comms→row 4 flag, Diag/Ui/ServicePump→bits only) | that service | no reset; service degraded per its own spec; visible in `ReliabilityState` | code `TaskStalled` latched |
| 7 | boot streak ≥ `kMaxRebootsInWindow` | `SafeMode` + code `BootLoop` | composition | W6 G1/P4: detection/publication only; selective service startup deferred | SensorFault event; exit = streak cleared by 10-min survival |
| 8 | reset reason = TWDT/brownout at boot | codes `WatchdogReset`/`Brownout` | — | latch + SensorFault event; feed row 7 streak | crash record updated |
| 9 | `CriticalBattery`/`LowBattery` (F23-reported) | bits only | — | F26 latches bits; power actions belong to F23/F25 (out of scope here) | none |
| 10 | `I2cTimeout`/`I2cBusStuck` | bits only; drives rows 1/2 outcomes | sensor paths | F01 recovers internally; `ReinitScheduler` (D12) governs reattempt pacing when an execution seam exists | none beyond bits |
| 11 | `wire()` `xTaskCreate*` != pdPASS (CR-033(a); F26's site = `SupervisorTask`, F12's `wire_ui.cpp` site = P4 composition item) | code `TaskCreateFailed` (27), bits only (no facility flag — nothing degraded *partially*) | F26 supervision itself | F26 cannot act: no heartbeat gating, no TWDT feed policy (the TWDT is initialised inside `SupervisorTask`), no supervisor ticks this boot; the composition keeps running unsupervised and the latch is the only trace of the silent no-feature boot | code latched in `fault_bits` (CR-030); severity **Critical** (severities note below) |
| 12 | `publishSensorFault`: sink absent or `publish()` != Ok (CR-033(b); F26's own SensorFault mirror — `AlertOutput`/F09 drops stay their owners' sites) | code `EventPublishDrop` (28), bits only | F17 history records (one `SensorFault` record lost) | drop the record, count it in `f26::droppedEvents()` **and** report the code; sensing and alerting are untouched — an event is a record, not an alert (00 non-negotiable holds) | code latched in `fault_bits`; severity **Warning** (severities note below) |

### 16.5 Supervision coverage (backlog 33 — explicit answer)
`DiagTask` (B), `StorageTask` (B), `CommsTask` (B), `ServicePump` (B) are all
supervised from W6 on, via the D8 beat lines added at P4. Expected periods:
Sensor 20 ms, Processing/Intelligence/State 1 000 ms, Alert 50 ms, Input 10 ms,
Ui 100 ms, Diag 1 000 ms, Storage 1 000 ms idle-poll, Comms idle-poll (beat per
wakeup, deadline `kStallFactor × 5 000 ms`), ServicePump 2 000 ms — all
`[EXPERIMENTAL]`, re-checked against the wire headers at IMPLEMENT.

IMPLEMENT re-check (2026-10-18): implemented verbatim as `kSupervisedTasks[]`
in `wire_reliability.cpp`, each period citing its wire-header constant;
`wire_hal.h` is ARDUINO-only, so the Sensor period is the literal 20 ms with a
citation comment. `kTierAMask`/`kTierBMask` are constexpr-derived from that one
table (never hand-maintained), and `kMaxSupervisedTasks` = 12 leaves one spare
slot for the next runtime.md §3.2 task.

**CR-029 — DegradedMode facility flags (bw_core)** · raised by F26 / 2026-10-18 ·
type **additive** · status: accepted 2026-10-18 (W6 G1), applied to `bw/providers.h`
- Problem: spec §3 produces "DegradedMode flags" (PPG lost / IMU lost / storage
  degraded / BLE down) but no `bw_core` vocabulary exists; consumers outside
  `bw_reliability` (F23 `degraded_mode` source, F24, F25, F12) may not include
  feature headers (01 §1 r1). `HealthSnapshot` has only a single bool.
- Proposal — append to `02_INTERFACES.md` §5 and `bw/providers.h`:
  ```cpp
  // CR-029 (F26): which capability is reduced. Append-only; in-memory only.
  enum class DegradedFacility : uint8_t {
      PpgLost = 0,      // PPG pipeline unusable (F01/F23 signal)
      ImuLost,          // IMU pipeline unusable
      StorageDegraded,  // F18 store refusing/losing data
      BleDown,          // sync capability unavailable (F19/F20)
      SafeMode,         // BootGuard safe mode active (F26)
      Count,            // sentinel; never stored as a flag
  };
  using DegradedMode = uint32_t;  // bit i set <=> DegradedFacility i active
  ```
- Alternatives: (a) no new type, consumers decode `fault_bits` — rejected:
  re-scatters D2's if/else and couples consumers to append-only code numbering;
  (b) observer/callback class — rejected: violates the value-type convention
  (02 §1). Affected: F26 (producer), F23 (degraded_mode source), F24/F25/F12.
- Migration: additive; `HealthSnapshot.degraded_mode` stays a bool summary.

**CR-030 — ReliabilityState read path into `HealthSnapshot.fault_bits`** ·
raised by F26 with F23's deferred item (F23 spec §15.8; `bw/fault.h:59`) ·
2026-10-18 · type **additive** · status: accepted 2026-10-18 (W6 G1), applied to
`bw/providers.h`
- Problem: `IFaultReporter` is write-only; F26's registry aggregates every
  report (incl. F23's own edges, which already flow via `ctx.faults`) but no
  contract lets F23 fill `fault_bits` with other features' codes.
- Proposal — append to `02_INTERFACES.md` §5 and `bw/providers.h`:
  ```cpp
  // CR-030 (F26 produces, F23 consumes). In-memory only, no kSchemaVersion.
  struct ReliabilityState {
      TimeMs t_ms;           // when produced
      uint32_t fault_bits;   // bit N <=> FaultCode value N reported at least
                             // once this boot; sticky, never cleared — same
                             // mapping as F23 D8, registry ⊇ F23's local bits
      DegradedMode degraded; // CR-029 facility mask
      bool safe_mode;        // BootGuard safe mode active
  };
  class IReliabilityStateProvider {
   public:
      virtual bool latest(ReliabilityState&) const = 0;  // false = no snapshot yet
  };
  ```
- Semantics: codes ≥ 32 (none today; values 0–24) are a defined no-op until a
  future CR defines a second word; no clearing (no `FaultCleared` code exists —
  recovery is visible via `SensorStatus` and events).
- Alternatives: (a) plain function accessor `f26::faultBits()` — rejected:
  re-opens the "no provider contract" gap for every future consumer, diverges
  from the CR-015/016/018 provider pattern; (b) move aggregation into F23 —
  rejected: 01 §7 names F26 the aggregator.
- Affected/migration: F23-full fills `health.fault_bits` from the provider when
  attached (local accumulation remains its pre-registry fallback); wiring lands
  at P4; F12 may read bits later. Producer seam in my wiring:
  `f26::latestReliabilityState(bw::ReliabilityState&)` (F23's D5 accessor
  precedent); F23-full consumes via its own attach step.

### 16.8 Test plan (host, `firmware/test/bw_reliability/test_bw_reliability/`)
1. `test_fault_registry.cpp` — counts/first/last/max-severity; sticky bits with
   the D4 mapping (golden: bit N ⇔ code N); out-of-range code no-op; first-call
   empty state; deterministic cross-task interleavings.
2. `test_supervisor.cpp` — all-fresh ⇒ feed; Tier A stall at 3× period ⇒ no
   feed; Tier B stall ⇒ flag, still feeding; grace window; pending (never
   beaten) vs stalled; huge `TimeMs` gaps.
3. `test_boot_guard.cpp` — 2 fast boots ⇒ normal, 3rd ⇒ safe mode; 10-min
   survival clears streak; missing/corrupt KV record ⇒ normal boot.
4. `test_degradation_policy.cpp` — **one case per §16.4 row** (1–10; rows
   11/12 are wiring-level CR-033 report sites, covered by items 7/10);
   absent
   provider ⇒ no flag (unknown ≠ down); link flap ⇒ hysteresis via
   `kBleDownConfirmMs`; never-connected BLE ⇒ no BleDown event.
5. `test_reinit_scheduler.cpp` — 1 s→2 s→4 s…→30 s cap; recovery resets.
6. `test_reliability_state_provider.cpp` — CR-030 seam contract: false before
   first tick; bits/degraded/safe_mode propagate; superset property vs a
   simulated F23 local accumulation.
7. `test_sensor_fault_events.cpp` — enter edge ⇒ one SensorFault (detector 26,
   payload pin); rate limit; re-edge after clear+re-enter; anomalous boot event.
8. `test_crash_record.cpp` — codec round-trip, schema mismatch ⇒ treated as
   absent, truncation.
9. `test_no_heap.cpp` — steady-state `step()` allocation counter.
10. `test_wire_reliability_compile.cpp` — wire pair compiles/drives on native
    (bw_ui/bw_sync precedent).
Estimate 45–55 new tests; **no existing test touched** (native floor 1260).

IMPLEMENT outcome (2026-10-18): **63 tests delivered** in the 10 files above
plus `reliability_test_support.h` (fakes + the `resetWiring()` seam defined in
the single wiring TU) — row 4 grew two extra cases (staleness and the
Comms-mapping half) and the wiring group covers items 3's KV half and 6/7 end
to end. Native floor held: **1260 → 1323, 0 removed/loosened.**

### 16.9 Budgets
- RAM: §11's < 2 KB holds — projected static ≈ 900 B (registry 25×24 B = 600 B,
  supervisor 12×16 B = 192 B, policy/crash state ≈ 100 B) + one
  `LatestValue<ReliabilityState>` ≈ 32 B; measured with `sizeof` asserts at
  Validate. Supervisor stack 4 096 B (FreeRTOS heap, recorded separately).
- CPU: one 1 Hz supervisor tick (O(25+12) scan) — negligible.

---
