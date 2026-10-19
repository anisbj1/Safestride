# `src/app/wiring/` — the wiring convention (01 §2, F00)

Every feature ships **exactly one pair** of files here:

```
firmware/src/app/wiring/wire_<name>.h
firmware/src/app/wiring/wire_<name>.cpp
```

`<name>` is the short name pinned by the feature's own spec `Owns:` list — both
forms exist in-tree and neither is being renamed: the library-style name
(`wire_hal` = F01, `wire_activity` = F03, `wire_wear` = F04, `wire_ml` = F05,
`wire_wake` = F06, `wire_cataplexy` = F09, `wire_alert` = F10) and the id form
(`wire_f02` = F02). What is invariant: **one pair
per feature**, and the C++ namespace inside is always the feature id
(`f01`, `f02`, … `f10`).
The pair is **owned by that feature** — it is the feature implementation's only footprint
under `src/app/`. `app.cpp`/`app.h` stay integrator-owned: features never edit them.

## Contract

```cpp
// wire_f02.h
#pragma once
namespace app {
struct AppContext;
}
namespace f02 {
// Constructs the feature's objects, injects dependencies from AppContext,
// registers/creates its task (if it has one). No behavior lives here.
void wire(app::AppContext& ctx);
}
```

- `void wire(AppContext&)` — construction and dependency injection only.
- If the feature owns a task (01 §3.2), its task function is declared here too,
  and its stack size / priority / core / period are **named constants with reasons**
  (`[HW]`/`[DERIVED]`/`[EXPERIMENTAL]`/`[USER]` — 00 §7.5), defined in this pair
  or the feature's own config, never inline magic numbers in `app.cpp`.
- Wiring contains **no logic**: no filtering, no state machines, no decisions —
  pure construction, injection and task creation (01 §1 rule 3).

## Call order

`app.cpp` calls the wire functions in one fixed order (01 §2), so merges into the
integrator-owned file stay one-line, conflict-free additions:

```
hardware -> processing -> intelligence -> decision -> services -> comms
(F01)      (F02)         (F03-F06)       (F07-F10,  (F11-F13,  F19/F20,
                                         F14)        F15-F18,  F28)
                                                   F22-F26)
```

F14 is numbered in the services range but wires in the **decision band**
(`wire_confirm.h`: "decision band, F07–F10 row" — its own header is the more
specific contract, DEC-021), so the decision row spans F07–F10 plus F14 and the
services band starts at F11.

### W6 composition exceptions / task adoption (P4, 2026-10-18)

- Construct F26's registry **before F01**, so every driver/feature caches the
  same fault reporter. This creates no supervisor task: attach KV, storage,
  BLE and event providers before `startSupervisor(ctx)`.
- F13 requires mounted F18 KV: load it in the services slot, then construct
  F06 with its `IBaselineProvider`. F07 can safely see an empty F06 mailbox
  during startup; it must not invent evidence.
- `task_health.h` is **application-level**, not another feature library.
  It maps each actual task to independent F23/F26 keys and handles CR-033
  task-creation/event-drop reports. Feature task shells call it after work,
  including legitimate empty-input cycles. Wake lateness is measured before
  work; monotonic timestamps use the same 64-bit ESP timer as DeviceClock.
- Preserve existing enum keys: `Intelligence` now means F03 only, `State`
  means F07 only; append wear/wake/sleep/cataplexy/log/baseline/research keys.
  F23 tracks the supervisor, which cannot supervise its own heartbeat.
- Integration ownership exception: expand **feature-local** F23/F26 registry
  capacities/configuration (19/18) and test the mappings in
  `firmware/test/test_wave6_dataflow`. No `bw_core` contract is changed.
  Static table expansion is simpler than a dynamic registry, avoids runtime
  allocation and preserves independently visible task failures.

- Mode-command relay: keep InputTask as the sole `ctx.commands` producer and
  AlertTask as its sole consumer. The latter converts ack/cancel to one-tick
  pulses and forwards only F24's four supported command types to F24's existing
  private SPSC, consumed by ServicePump. No task writes back to `ctx.commands`.
  Full/absent mode sink refuses without retry; `mode_drops` exposes refusals.
  BLE commands remain unbound (not a second producer on either queue).
- F07 ownership exception (P4): add only an optional `IModeProvider` port and
  pure-pump mode/power stamping in `bw_state`; no engine policy, thresholds,
  gates or shared contracts change. Absent/unpublished = Day/Normal. Bind all
  F07 ports **before** `wire(ctx)` starts StateTask; same-core scheduling does
  not make concurrent plain-pointer writes safe.
- Simplest option: reuse the two existing SPSC queues and CR-027 provider.
  Scalable option: a central multi-producer command broker and generic runtime
  port-rebinding mechanism. The latter adds lifecycle/ordering complexity and
  RAM for no W6 requirement; choose the existing seams, startup-only binding.

- Final P4 lifetime rule: every one of the 19 task shells calls
  `tasks::waitForStartup()` before reading the graph. The composition releases
  the one-way atomic barrier after all attachments, task creation attempts and
  startup actuator setup. Release/acquire publishes plain startup ports on both
  cores; paced 10 ms startup waits avoid spinning. A permanent wiring failure
  does not invent a heartbeat. Runtime rebinding is not supported by this gate.
  Simplest: one startup barrier in the existing task-adoption seam. Scalable:
  split every wire function into construction/start phases; rejected for this
  wave because it duplicates lifecycle entry points across all features.
- P4 safe-mode ruling (G1 item ④ permits this choice): **detection/publication
  only in W6**. Keep the full graph wired, including immediate offline alerts;
  publish BootLoop/SafeMode through F26→F23 and device diagnostics. Selective
  boot suppression and a dedicated F12 safe screen are deferred explicitly.
  Early services are required to read boot history, and a transient history
  failure can discover safe mode later; live task teardown would add lifecycle
  complexity unrelated to the approved W6 detection contract. Do not claim
  that optional services are disabled or that reboot loops are prevented.
- F13→F06 absent/untrained/restored injection and held F19 event-time→F26→F23
  disconnect/recovery H8 now run in `test_wave6_dataflow`. The boot-loop case
  proves reporting and unchanged local F10 demand, not a device reset trial.
- The lifetime review also replaces bench reads of mutable HAL sensor statuses
  with F23's published copy, and makes F10's bench-read event-drop count atomic.
  Barrier adoption and these wiring-only changes are integration-owned exceptions.

A feature that depends on another's instance gets it **through `AppContext`**
(a provider pointer or a mailbox), never by including the other feature's headers
(01 §1 rule 1: compile-time dependencies point to `bw_core` only).

## F00 ships no `wire_f00.*`

F00 has no feature logic (F00 §5) — the convention is documented here, not
instantiated. The first pair lands with F01 (or whichever feature comes first
after M0 integration).

## Checklist for the feature implementation

1. Create only your own `wire_<feature>.{h,cpp}` — never touch `app.cpp`,
   `app.h` or another feature's wiring file.
2. Your `wire()` compiles for `esp32dev` and `native` where meaningful (DoD §4.8).
3. Everything you need comes from `AppContext` or your constructor arguments —
   if a needed contract type is missing, raise a change request (03 §5), do not
   include a peer feature's header.
4. Update this README only if the *convention itself* changes (integrator call).
