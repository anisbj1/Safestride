#include <bw/reliability/crash_record.h>

namespace bw::reliability {

namespace {

void putLe32(uint8_t* out, const uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFFu);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t getLe32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

// Largest legal ResetReason value (append-only vocabulary).
constexpr uint8_t kMaxResetReason = static_cast<uint8_t>(ResetReason::DeepSleep);

}  // namespace

// Layout (schema 1):
//   [0]  schema   [1] streak   [2] reset_reason   [3] reserved
//   [4..7]  boot_id (LE)       [8..11] fault_bits (LE)   [12..15] reserved
size_t encodeCrashRecord(const CrashRecord& rec, uint8_t* out, const size_t cap) {
    if (out == nullptr || cap < kCrashRecordBytes) {
        return 0;  // caller budget too small, nothing written
    }
    for (size_t i = 0; i < kCrashRecordBytes; ++i) {
        out[i] = 0;  // reserved bytes stay 0 (forward-compatible readers skip them)
    }
    out[0] = kCrashRecordSchema;
    out[1] = rec.streak;
    out[2] = static_cast<uint8_t>(rec.last_reset_reason);
    putLe32(out + 4, rec.boot_id);
    putLe32(out + 8, rec.fault_bits_at_boot);
    return kCrashRecordBytes;
}

bool decodeCrashRecord(const uint8_t* buf, const size_t len, CrashRecord& out) {
    if (buf == nullptr || len < kCrashRecordBytes) {
        return false;  // truncated / absent
    }
    if (buf[0] != kCrashRecordSchema) {
        return false;  // different schema: not ours to interpret
    }
    if (buf[2] > kMaxResetReason) {
        return false;  // corrupt enum byte: absent beats a guess (00 §1)
    }
    out.streak = buf[1];
    out.last_reset_reason = static_cast<ResetReason>(buf[2]);
    out.boot_id = getLe32(buf + 4);
    out.fault_bits_at_boot = getLe32(buf + 8);
    return true;
}

}  // namespace bw::reliability
