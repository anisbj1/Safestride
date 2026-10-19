# CR-031 — Append `FaultCode::BaselineCorrupt` (= 25)

- **Date:** 2026-10-18 · **Wave:** W6 G1 · **Raised by:** F13 (PLAN, W6) · **Decided by:** integrator
- **Type:** append-only enum (`bw/fault.h`; values 0–24 taken, next free = **25**)
- **Status: accepted + applied 2026-10-18** (applied on `master`, G1)

## Problem
F13 persists a 32-byte baseline blob (`IKeyValueStore`, key `f13.baseline`) with a
version tag and CRC. A corrupt or version-incompatible blob is discarded and learning
restarts — but the discard was invisible: no `FaultCode` fits "stored baseline
corrupt", and the enum is append-only via CR (`bw/fault.h` header rule).

## Proposal
Append to `FaultCode` after `BleDown` (value **25**), in a new `── personalization ──`
block:

```cpp
    BaselineCorrupt,  // F13 §10 / CR-031: stored baseline profile corrupt or
                      // version-incompatible -> discarded, learning restarted
```

Severity is deliberately **not** pinned in the enum (F00 §16 rule): the mapping
belongs to F26's degradation table; F13 reports it as `Warning` ("still functioning,
learning restarts from zero").

## Alternatives
- (a) Silent discard (no code) — rejected: invisible data loss; F23's health would
  never see it and the honest-degradation rule requires a visible cause.
- (b) Reuse `StorageDegraded` (23) — rejected: that code means F18's store is
  refusing/losing data; conflating a 32-byte blob CRC failure with a storage fault
  would send F26's degradation policy down the wrong branch.

## Trade-offs
One more value in a 32-bit `fault_bits` word (26 of 32 used); no consumer reads it
yet. Accepted: cheap, and F23's CR-030 read path picks it up automatically
(bit 25).

## Registry effect
`FaultCode` next free → **26**. F13's detector_id stays unused (no event emitted);
CR numbers consumed this wave: 027, 028 (rejected), 029, 030, 031 → next free
**CR-032**.
