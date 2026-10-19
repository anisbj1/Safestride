# CR-032 — Append `FaultCode::LowStack` (= 26)

- **Date:** 2026-10-18 · **Wave:** W6 G1 (batch 2) · **Raised by:** F23 (PLAN, M5/full slice) · **Decided by:** integrator
- **Type:** append-only enum (`bw/fault.h`; after `BaselineCorrupt` = 25, next free = **27**)
- **Status: accepted + applied 2026-10-18** (applied on `master`, G1)

## Problem
The M5 slice adds per-task stack watermarks (deep overload analysis, spec §15.9).
A low watermark had no vocabulary: `LowHeap` (16) covers heap only, and `FaultCode`
is append-only via CR. Without a code the watermarks would be readable through
`f23::latestTaskHealth` but never *diagnosable*.

## Proposal
Append to `FaultCode` after `BaselineCorrupt` (value **26**):

```cpp
    // ── system overload (F23 M5, CR-032) ───────────────────────────────────
    LowStack,  // F23 M5: TaskHealth.stack_free_min_bytes < kMinTaskStackFreeBytes
```

Severity is policy (F00 §16 rule): F23's degradation table assigns **Warning** —
"still functioning, grow that task's stack"; the affected task is identified via
`f23::latestTaskHealth`, not by the code; F26 owns the system response.

## Alternatives
- (a) Reuse `TaskOverrun` (15) — rejected: that is lateness-based overload of the
  pipeline; a starved stack is a different symptom with a different remedy, and
  conflating them hides both.
- (b) No code, expose watermarks only — rejected (see Problem).

## Trade-offs
Uses value 26 of the 32-bit `fault_bits` word (27 of 32 including `None`); one more
row in the pin test. Accepted: cheap, append-only, and the CR-030 read path carries
it to `HealthSnapshot.fault_bits` with no further contract work.

## Registry effect
`FaultCode` next free → **27**. CR numbers consumed in W6: 027–032 (028 rejected and
burned) → next free **CR-033**.
