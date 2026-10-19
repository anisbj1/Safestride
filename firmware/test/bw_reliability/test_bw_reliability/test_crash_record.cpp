// bw_reliability — crash-record codec tests (spec §16.8 item 8): round trip,
// the schema tag and the "absent beats a guess" rules (short buffer, wrong
// schema, corrupt enum byte — 00 §1: unknown is a valid output).

#include <cstdint>

#include <bw/reliability/boot_guard.h>
#include <bw/reliability/crash_record.h>
#include <bw/reliability/reliability_config.h>

#include <unity.h>

namespace {

using bw::reliability::CrashRecord;
using bw::reliability::ResetReason;

}  // namespace

// Round trip: every field survives the persistence boundary and the buffer is
// exactly kCrashRecordBytes (<= D11's 32 B budget).
void test_crash_record_round_trip(void) {
    CrashRecord in{};
    in.streak = 2;
    in.last_reset_reason = ResetReason::TaskWatchdog;
    in.boot_id = 0xDEADBEEFu;
    in.fault_bits_at_boot = 0x00C00000u;  // StorageDegraded + BleDown (codes 23/24)

    uint8_t buf[bw::reliability::kCrashRecordBytes];
    const size_t written = bw::reliability::encodeCrashRecord(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kCrashRecordBytes, written);
    TEST_ASSERT_EQUAL_UINT8(bw::reliability::kCrashRecordSchema, buf[0]);  // schema tag first

    CrashRecord out{};
    TEST_ASSERT_TRUE(bw::reliability::decodeCrashRecord(buf, written, out));
    TEST_ASSERT_EQUAL_UINT8(in.streak, out.streak);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(in.last_reset_reason),
                          static_cast<int>(out.last_reset_reason));
    TEST_ASSERT_EQUAL_UINT32(in.boot_id, out.boot_id);
    TEST_ASSERT_EQUAL_HEX32(in.fault_bits_at_boot, out.fault_bits_at_boot);
}

// Argument error: a buffer under kCrashRecordBytes writes nothing and
// returns 0 (caller budget too small — never a partial record).
void test_crash_record_encode_small_buffer_writes_nothing(void) {
    CrashRecord in{};
    in.streak = 1;
    uint8_t buf[bw::reliability::kCrashRecordBytes + 4] = {0xAA, 0xAA, 0xAA, 0xAA};
    TEST_ASSERT_EQUAL_UINT32(0, bw::reliability::encodeCrashRecord(in, buf, 3));
    TEST_ASSERT_EQUAL_UINT8(0xAA, buf[0]);  // untouched
    TEST_ASSERT_EQUAL_UINT32(0, bw::reliability::encodeCrashRecord(in, nullptr, 64));
}

// Schema mismatch => treated as ABSENT (core.md §1 versioning): a future
// layout must never be decoded as today's fields.
void test_crash_record_schema_mismatch_is_absent(void) {
    CrashRecord in{};
    in.streak = 3;
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kCrashRecordBytes,
                             bw::reliability::encodeCrashRecord(in, buf, sizeof(buf)));
    buf[0] = static_cast<uint8_t>(bw::reliability::kCrashRecordSchema + 1);

    CrashRecord out{};
    out.streak = 0xCC;  // must remain untouched — no partial decode
    TEST_ASSERT_FALSE(bw::reliability::decodeCrashRecord(buf, sizeof(buf), out));
    TEST_ASSERT_EQUAL_UINT8(0xCC, out.streak);
}

// Truncation and a corrupt ResetReason byte are both "no record": the caller
// falls back to a normal boot (streak base 0) instead of trusting garbage.
void test_crash_record_truncated_or_corrupt_is_absent(void) {
    CrashRecord in{};
    in.streak = 2;
    uint8_t buf[bw::reliability::kCrashRecordBytes];
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kCrashRecordBytes,
                             bw::reliability::encodeCrashRecord(in, buf, sizeof(buf)));

    CrashRecord out{};
    TEST_ASSERT_FALSE(bw::reliability::decodeCrashRecord(buf, sizeof(buf) - 1, out));  // short
    TEST_ASSERT_FALSE(bw::reliability::decodeCrashRecord(nullptr, sizeof(buf), out));  // absent
    buf[2] = static_cast<uint8_t>(ResetReason::DeepSleep) + 1;  // unknown enum byte
    TEST_ASSERT_FALSE(bw::reliability::decodeCrashRecord(buf, sizeof(buf), out));
}
