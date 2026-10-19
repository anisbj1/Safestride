# CR-033 — Append `FaultCode::TaskCreateFailed` (= 27) and `FaultCode::EventPublishDrop` (= 28)

- **Date:** 2026-10-18 · **Wave:** W6 P2 (mid-IMPLEMENT) · **Raised by:** F26 (IMPLEMENT report + spec status log; realizes G1 backlogs 16/18) · **Decided by:** integrator
- **Type:** append-only enum (`bw/fault.h`; after `LowStack` = 26, next free = **29**)
- **Status: accepted + applied 2026-10-18** (applied on `master`, W6 P2)

## Problem
Two wiring failure modes named by development documentation backlogs 16 and 18 have no
vocabulary, and `FaultCode` is append-only via CR (02 §6.6):

- **(a) Task-creation failure** — `wire_*` ignores `xTaskCreatePinnedToCore`
  failure (silent no-UI/no-feature on boot; backlog 16). F26's IMPLEMENT had to
  latch the closest-in-range `TaskStalled` (19) instead — but a task that never
  started never had a heartbeat, so the symptom is conflated with a stall of a
  running task and the supervisor's beat logic can never detect it.
- **(b) Event-sink publish drop** — `AlertOutput::publish_err` / F09 event drops
  are counted in wiring but never reported (backlog 18: "fold into F26's code
  appends"). Today they are only visible via `f26::droppedEvents()`, not through
  `ctx.faults` / `ReliabilityState.fault_bits` (CR-030).

## Proposal
Append to `FaultCode` after `LowStack` (values **27**, **28**):

```cpp
    // ── composition wiring (F26 backlogs 16/18, CR-033) ────────────────
    TaskCreateFailed,  // wire() xTaskCreate* != pdPASS: a task never started this boot
    EventPublishDrop,  // event sink missing/rejected a published event (counted at the site)
```

Severity is policy (F00 §16 rule) and is chosen **at the report site**: the
reporter already passes `FaultSeverity` per call, so the wiring that observes
the failure picks the level for the affected task/feature (e.g. a UI-only task
is not alert-critical; a drop of an event is data loss ⇒ Warning). F26's
degradation table records the rows.

## Alternatives
- (a) Keep the `TaskStalled` stand-in — rejected: conflates "never started"
  with "stopped beating" (backlogs 16/18 explicitly deferred the append to the
  F26 wave; the stand-in hides both symptoms).
- (b) Reuse `TaskOverrun` (15) / `LowHeap` (16) — rejected: different symptoms,
  different remedies; conflating them is exactly what append-only CRs exist to
  prevent.
- (c) Counters only, no code — rejected: invisible to `fault_bits` consumers
  (CR-030 read path) and to F23's health aggregation.

## Trade-offs
Uses values 27–28 of the 32-bit `fault_bits` word (29 of 32 including `None`)
and two more pin-test rows. Accepted: cheap, append-only, and the CR-030 read
path carries both codes to `HealthSnapshot.fault_bits` with no further contract
work. `app.cpp`'s `faultCodeName` switch gains the 4 missing labels
(25–28) in the same commit, removing the `-Wswitch` warnings G1 left behind.

## Registry effect
`FaultCode` next free → **29**. CR numbers consumed in W6: 027–033 (028 rejected
and burned) → next free **CR-034**.
