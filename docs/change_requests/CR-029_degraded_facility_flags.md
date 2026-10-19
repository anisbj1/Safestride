# CR-029 — `DegradedFacility` / `DegradedMode` capability flags

- **Date:** 2026-10-18 · **Wave:** W6 G1 · **Raised by:** F26 (PLAN, W6) · **Decided by:** integrator
- **Type:** additive (append to `bw/providers.h` + `services.md` §5)
- **Status: accepted + applied 2026-10-18** (applied on `master`, G1)

## Problem
F26's spec §3 produces "DegradedMode flags" (PPG lost / IMU lost / storage degraded /
BLE down) but no `bw_core` vocabulary exists for them. Consumers outside
`bw_reliability` (F23's degraded-mode source, later F25/F12/F24) may depend on
`bw_core` only (01 §1 r1). `HealthSnapshot.degraded_mode` is a single bool and cannot
say *which* capability is down; decoding `fault_bits` instead would re-scatter
per-feature if/else and couple consumers to the append-only code numbering.

## Proposal
Append to `bw/providers.h`:

```cpp
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

## Alternatives
- (a) No new type; consumers decode `fault_bits` — rejected (see Problem).
- (b) Observer/callback class — rejected: violates the value-type convention (02 §1).

## Trade-offs
A facility flag and a `FaultCode` describe overlapping situations (e.g. `BleDown`
code 24 vs `DegradedFacility::BleDown`) in two vocabularies. Accepted and documented
at the enum: a **facility** names the capability that is down, a **code** names the
symptom that was reported — consumers pick by intent, F26 produces both from one
internal state so they cannot disagree.

## Migration / affected
Additive; `HealthSnapshot.degraded_mode` keeps its bool summary (no consumer change).
Producer F26 (W6); first consumer F23-full (same wave, CR-030).
