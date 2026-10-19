# 01 — ARCHITECTURE

> Read after `00_PROJECT_CONTEXT.md`. Version 1.0. Tags: **[DECIDED]**, **[ASSUMED]**, **[OPEN]**.
> Numeric values here are `[EXPERIMENTAL]` unless tagged otherwise; they are starting points, not facts.

---


## 1. Layering rules

Dependencies point **downward only**. A layer may use layers below it, never above, and never sideways except through `bw_core` contracts.

```
L5  app/            composition root, task creation, wiring (no logic)
L4  services        alert, ui, input, eventlog, storage, ble, sync, modes, power, reliability, ota, research
L3  decision        state engine, sleep episodes, event detectors (F09), nap manager, confirmation
L2  intelligence    feature assembly + estimators (F06), ML runtime (F05), baseline (F13), activity (F03), wear (F04), diagnostics (F23)
L1  processing      signal processing (F02)
L0  hardware        HAL drivers (F01)
--  bw_core         contracts, types, containers, Result, interfaces. Depended on by everyone; depends on nothing.
```

Hard rules:
1. **Compile-time**: a feature library includes only `bw_core` headers (and the C++ standard library / its own files). Never another feature's headers.
2. **Run-time**: features talk through `bw_core` value types passed via mailboxes/queues (§3) and through read-only provider interfaces (`IStateProvider`, `IEventStore`, `ISettings`, ...).
3. **Only `src/app/` knows concrete classes.** It instantiates and wires them (dependency injection by constructor).
4. **No feature reads another feature's storage**; persistence goes through `IKeyValueStore` / `IEventStore`.
5. **Pure logic has no platform calls.** Anything in L1–L3 (and most of L4) is `Output step(const Input&)`-style, compiled for `native` and `esp32dev`.

---

## 7. Error handling
`Result<T>` with `Err` codes everywhere; no exceptions, no `abort()` in normal operation. Faults are reported via `IFaultReporter` (F26 aggregates). A missing/failed source yields `DataQuality::Insufficient`, never a guess.

## 8. Data handling (minimal)
Local-first: no cloud or server. BLE links use standard bonding (F19). Firmware images are hash-checked before switching (F30). No further privacy or security tooling is in scope (DEC-019).

## Where the rest lives (open only if your spec says so)
| Sections | File | Needed by |
|---|---|---|
| §3 runtime/task model, §4 time model | `architecture/runtime.md` | features with tasks, timing or wall-clock use |
| Repository layout, stack, and test strategy | `architecture/layout_stack_tests.md` |
