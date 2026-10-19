// F18 §15.5 KvStore tests (T9: D7 per-key files, CRC discard, atomic rename).

#include <cstring>

#include <unity.h>

#include <bw/testkit/recording_fault_reporter.h>

#include <bw/storage/kv_store.h>

#include "ram_fs_fake.h"

using bw::storage::KvStore;
using bw::storage::StorageCounters;

namespace {

struct KvFixture {
    RamFsFake fs{64u * 1024u};
    StorageCounters counters{};
    bw::testkit::RecordingFaultReporter<8> faults;
    KvStore kv{fs, counters};

    KvFixture() {
        TEST_ASSERT_TRUE(fs.mount(true).ok());
        kv.attachFaultReporter(&faults);
    }
};

}  // namespace

void test_kv_put_get_roundtrip(void) {
    KvFixture f;
    const char value[] = "hello-storage";
    TEST_ASSERT_TRUE(f.kv.put("greeting", value, sizeof(value)).ok());
    char out[32] = {};
    auto got = f.kv.get("greeting", out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(sizeof(value), got.value);
    TEST_ASSERT_EQUAL_STRING(value, out);
}

void test_kv_missing_key_returns_not_found(void) {
    KvFixture f;
    char out[8];
    auto got = f.kv.get("nope", out, sizeof(out));
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::NotFound), static_cast<uint8_t>(got.err));
}

void test_kv_roundtrip_across_reboot(void) {
    KvFixture f;
    const uint32_t value = 0xDEADBEEF;
    TEST_ASSERT_TRUE(f.kv.put("counter", &value, sizeof(value)).ok());
    // "Reboot": fresh store over the same medium.
    KvStore again{f.fs, f.counters};
    uint32_t out = 0;
    auto got = again.get("counter", &out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_HEX32(value, out);
}

void test_kv_corrupt_blob_discards_and_counts(void) {
    KvFixture f;
    const char value[] = "persisted";
    TEST_ASSERT_TRUE(f.kv.put("setting", value, sizeof(value)).ok());
    f.fs.corruptByte("kv_setting.bin", 12);  // inside the payload
    char out[32];
    auto got = f.kv.get("setting", out, sizeof(out));
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Corrupt), static_cast<uint8_t>(got.err));
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.kv_discards);
    TEST_ASSERT_EQUAL_UINT32(1, f.faults.countOf(bw::FaultCode::StorageDegraded));
}

void test_kv_torn_put_leaves_old_value(void) {
    KvFixture f;
    const uint32_t old_value = 111;
    const uint32_t new_value = 222;
    TEST_ASSERT_TRUE(f.kv.put("level", &old_value, sizeof(old_value)).ok());
    // Tear the next write = the tmp header of the second put: rename never
    // runs, the destination still holds the OLD value (D7).
    f.fs.armTornWrite(3);
    TEST_ASSERT_FALSE(f.kv.put("level", &new_value, sizeof(new_value)).ok());
    uint32_t out = 0;
    auto got = f.kv.get("level", &out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_HEX32(old_value, out);
}

void test_kv_invalid_keys_rejected(void) {
    KvFixture f;
    char out[8];
    const char* bad[] = {"", "with/slash", "with space", "a\nb", "tab\there"};
    for (const char* key : bad) {
        auto got = f.kv.get(key, out, sizeof(out));
        TEST_ASSERT_FALSE(got.ok());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::InvalidArg),
                                static_cast<uint8_t>(got.err));
        TEST_ASSERT_FALSE(f.kv.put(key, out, 1).ok());
    }
    // Exactly kKvKeyMaxLen chars is fine, one more is not.
    const char len32[] = "01234567890123456789012345678901";  // 32
    TEST_ASSERT_TRUE(f.kv.put(len32, out, 1).ok());
    const char len33[] = "012345678901234567890123456789012";
    TEST_ASSERT_FALSE(f.kv.put(len33, out, 1).ok());
}

void test_kv_oversize_value_rejected(void) {
    KvFixture f;
    char big[bw::storage::kKvMaxValueBytes + 1] = {};
    TEST_ASSERT_FALSE(f.kv.put("blob", big, sizeof(big)).ok());
    TEST_ASSERT_FALSE(f.fs.exists("kv_blob.bin"));
}

void test_kv_small_buffer_returns_full_untouched(void) {
    KvFixture f;
    const char value[] = "0123456789";
    TEST_ASSERT_TRUE(f.kv.put("data", value, sizeof(value)).ok());
    char out[4] = {'x', 'x', 'x', 'x'};
    auto got = f.kv.get("data", out, sizeof(out));
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Full), static_cast<uint8_t>(got.err));
    // Refused, every byte untouched (CR-004). Byte-wise: out has no NUL, so
    // a string compare would read past the buffer and lie.
    const char untouched[4] = {'x', 'x', 'x', 'x'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, out, 4);
}

void test_kv_stale_tmp_removed_on_next_put(void) {
    KvFixture f;
    // Simulate a power cut that left kv_x.tmp behind.
    const std::vector<uint8_t> garbage = {1, 2, 3, 4, 5};
    f.fs.setBytes("kv_retry.tmp", garbage);
    TEST_ASSERT_TRUE(f.fs.exists("kv_retry.tmp"));
    const uint8_t value = 9;
    TEST_ASSERT_TRUE(f.kv.put("retry", &value, sizeof(value)).ok());
    TEST_ASSERT_FALSE(f.fs.exists("kv_retry.tmp"));
    uint8_t out = 0;
    TEST_ASSERT_TRUE(f.kv.get("retry", &out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_UINT8(9, out);
}

void test_kv_empty_value_roundtrip(void) {
    KvFixture f;
    TEST_ASSERT_TRUE(f.kv.put("empty", nullptr, 0).ok());
    char out[4] = {'z', 'z', 'z', 'z'};
    auto got = f.kv.get("empty", out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(0, got.value);
    // len 0: nothing copied (not even a NUL), no error — byte-wise assert,
    // out is not a terminated string.
    const char untouched[4] = {'z', 'z', 'z', 'z'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(untouched, out, 4);
}

void test_kv_get_crc_validates_after_overwrite(void) {
    KvFixture f;
    const uint16_t a = 1000;
    const uint16_t b = 2000;
    TEST_ASSERT_TRUE(f.kv.put("u", &a, sizeof(a)).ok());
    TEST_ASSERT_TRUE(f.kv.put("u", &b, sizeof(b)).ok());  // atomic overwrite
    uint16_t out = 0;
    auto got = f.kv.get("u", &out, sizeof(out));
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT16(b, out);
    TEST_ASSERT_EQUAL_UINT32(0, f.counters.kv_discards);
}
