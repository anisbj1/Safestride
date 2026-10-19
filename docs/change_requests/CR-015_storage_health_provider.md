# CR-015 — Storage health contract (`StorageHealth` + `IStorageHealthProvider`)

- **Raised by:** F18 (plan stage, wave 3), 2026-10-12 — integrator files.
- **Status:** **accepted + applied** 2026-10-12 (integrator, W3 G1; additive, integrator authority per docs/05 §6).
- **Applies to:** `docs/interfaces/services.md` §5, `firmware/lib/bw_core/include/bw/providers.h` (append-only).

## Motivation

F18's spec §3 requires free space + corruption counters readable by F23. F23 may not
include `bw_storage` headers (01 §1 rule 1). `HealthSnapshot` (services.md §5) has no
storage fields, and grep of `bw_core` finds no storage-health type. No existing contract
carries these numbers; F18 cannot define them in `bw_core` itself.

## Change (append-only; no existing type modified)

```cpp
// bw/providers.h — F18 produces, F23 (and diagnostics) consume.
// In-memory only, never persisted or transmitted → no kSchemaVersion (core.md §1).
struct StorageHealth {
    TimeMs t_ms;                  // when produced
    uint32_t total_bytes;
    uint32_t free_bytes;
    uint32_t corrupt_records;
    uint32_t torn_tails_repaired;
    uint32_t segments_skipped;
    uint32_t evicted_synced_segments;
    uint32_t evicted_windows;
    uint32_t kv_discards;
};
class IStorageHealthProvider {
public:
    virtual bool latest(StorageHealth&) const = 0;  // false = no snapshot yet
};
```

`StorageHealth` is a plain aggregation of F18's existing counters; implementation is a
thin provider view over the store (F18 owns the concrete class; wiring hands the pointer
to F23 at P4).
