#pragma once

// bw/reliability/crash_record.h — the single fixed KV record of spec D11
// (<= 32 B, schema-tagged; core.md §1 versioning). No history ring: storage
// churn on a crashing device is a hazard and F18 owns the FS lifecycle (D11)
// — the scalable option (a multi-entry crash ring) is deferred until
// post-mortem history beyond the last crash is actually needed.
//
// Byte layout is THIS file's contract (explicit little-endian fields — never
// a struct memcpy across the persistence boundary).

#include <cstddef>
#include <cstdint>

#include <bw/reliability/boot_guard.h>  // ResetReason (crash_record stores it)
#include <bw/reliability/reliability_config.h>

namespace bw::reliability {

struct CrashRecord {
    uint8_t streak = 0;                                    // consecutive short-lived boots
    ResetReason last_reset_reason = ResetReason::Unknown;  // why this boot started
    uint32_t boot_id = 0;                                  // 0 = unknown: no F17 bootId()
                                                           // accessor exists at wiring yet
                                                           // (wire_research.cpp:42 precedent)
    uint32_t fault_bits_at_boot = 0;                       // CR-030 word at write time
};

// [DERIVED] Why 16 B: schema(1) + streak(1) + reason(1) + reserved(1) +
// boot_id(4) + fault_bits(4) + reserved(4) = 16 <= D11's 32 B budget, fixed
// for the lifetime of schema 1.
constexpr size_t kCrashRecordBytes = 16;

// Returns bytes written, or 0 when `cap` < kCrashRecordBytes (nothing written).
size_t encodeCrashRecord(const CrashRecord& rec, uint8_t* out, size_t cap);

// false => treat the value as ABSENT (spec §16.8 test 8): short buffer, schema
// mismatch, or an unknown ResetReason byte (corrupt — never decoded into a
// guess).
bool decodeCrashRecord(const uint8_t* buf, size_t len, CrashRecord& out);

}  // namespace bw::reliability
