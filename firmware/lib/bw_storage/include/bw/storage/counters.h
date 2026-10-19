#pragma once

// bw/storage/counters.h — F18's health counters: the CR-015 fields surfaced
// through bw::IStorageHealthProvider, plus fault-only fields that are NOT
// part of CR-015's locked StorageHealth (format_recoveries — its F23-visible
// surface is the StorageDegraded fault reported at the same moment; audit
// F-03/NEW-L2).
//
// One instance is shared by all F18 modules (event store, KvStore,
// WindowStore). Wired once at composition-root time and passed by reference;
// never heap-allocated, never copied (plain struct, no atomics: every writer
// and reader runs in StorageTask context or accepts benign staleness for
// diagnostics).

#include <cstdint>

namespace bw::storage {

struct StorageCounters {
    // Records skipped because magic/len/CRC/seq failed mid-segment (D4:
    // forward resync) and window/KV blobs discarded on the same rule.
    uint32_t corrupt_records;
    // Torn tails truncated back to the last good record boundary at mount or
    // after a failed runtime append (D4).
    uint32_t torn_tails_repaired;
    // Whole segments skipped at boot (bad header, resync budget exhausted, or
    // a transient probe IO failure — the latter are KEPT on disk, F-04).
    uint32_t segments_skipped;
    // Segments ring-overwritten while fully synced (D6 order step 1).
    uint32_t evicted_synced_segments;
    // Windows dropped oldest-first under space pressure (D6 order step 2).
    uint32_t evicted_windows;
    // KV blobs that failed CRC/version check and were discarded for the
    // caller to restart from defaults (D7).
    uint32_t kv_discards;
    // Mount fell back to formatting because the read-only mount failed
    // (F-03 two-step mount): the medium needed provisioning (first boot) or
    // was unreadable — in the latter case any prior data is gone. Counted
    // only when the fallback format actually ran. Not a CR-015 StorageHealth
    // field (contract locked): the StorageDegraded fault reported alongside
    // is the F23-visible surface.
    uint32_t format_recoveries;
};

}  // namespace bw::storage
