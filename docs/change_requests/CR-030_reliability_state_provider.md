# CR-030 — `ReliabilityState` / `IReliabilityStateProvider` (fault-registry read path)

- **Date:** 2026-10-18 · **Wave:** W6 G1 · **Raised by:** F26 (PLAN, W6) with F23's
  deferred item (F23 spec §15.8; gap recorded in `bw/fault.h`) · **Decided by:** integrator
- **Type:** additive (append to `bw/providers.h` + `services.md` §5)
- **Status: accepted + applied 2026-10-18** (applied on `master`, G1)

## Problem
`IFaultReporter` is write-only: features report codes, F26's registry aggregates them,
but nothing lets F23 fill `HealthSnapshot.fault_bits` with *other* features' codes.
This is the gap F00 recorded when it declared `IFaultReporter`, and F23 deferred it to
W6 with F26 (spec §15.8).

## Proposal
Append to `bw/providers.h` (depends on CR-029's `DegradedMode`):

```cpp
struct ReliabilityState {
    TimeMs t_ms;            // when produced
    uint32_t fault_bits;    // bit N <=> FaultCode value N reported at least once
                            // this boot; sticky, never cleared — same mapping as
                            // F23 D8; the registry is a superset of F23's own bits
    DegradedMode degraded;  // CR-029 facility mask
    bool safe_mode;         // BootGuard safe mode active
};
class IReliabilityStateProvider {
 public:
    virtual bool latest(ReliabilityState&) const = 0;  // false = no snapshot yet
};
```

Semantics: codes ≥ 32 are a defined no-op until a future CR defines a second word
(`kFaultBitsWidth` is 32; today's values are 0–25); no clearing exists — no
`FaultCleared` code is in the enum, and recovery is visible through `SensorStatus`
and events.

## Alternatives
- (a) Plain accessor `f26::faultBits()` — rejected: re-opens the "no provider
  contract" gap for every future consumer and diverges from the CR-015/016/018
  provider pattern.
- (b) Move aggregation into F23 — rejected: `01_ARCHITECTURE.md` §7 names F26 the
  fault aggregator.

## Trade-offs
Sticky-since-boot bits cannot express "recovered"; accepted — clearing needs a code
and a policy that no spec owns yet, and consumers that need recovery have
`SensorStatus`. F23 keeps its local accumulation as the fallback when no provider is
attached, so ordering at wiring cannot fabricate health.

## Migration / affected
Additive, no wire/schema impact. Producer F26 (W6); consumer F23-full (W6, same wave,
consumer-side attach step in F23's plan); wiring bind by the integrator at P4.
