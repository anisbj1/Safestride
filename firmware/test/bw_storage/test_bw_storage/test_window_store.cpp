// F18 §15.5 WindowStore tests (T10: int16 blobs tied to seq, oldest-first
// eviction, torn/corrupt blob discard).

#include <unity.h>

#include <bw/testkit/recording_fault_reporter.h>

#include <bw/storage/window_store.h>

#include "ram_fs_fake.h"

using bw::storage::StorageCounters;
using bw::storage::WindowStore;

namespace {

// 100 samples = 200 B payload — small enough for tight-capacity tests.
constexpr size_t kSamples = 100;

struct WindowFixture {
    RamFsFake fs;
    StorageCounters counters{};
    bw::testkit::RecordingFaultReporter<8> faults;
    WindowStore windows{fs, counters};

    explicit WindowFixture(uint32_t capacity = 256u * 1024u) : fs(capacity) {
        TEST_ASSERT_TRUE(fs.mount(true).ok());
        windows.attachFaultReporter(&faults);
    }
};

std::vector<int16_t> makeSamples(uint32_t seq) {
    std::vector<int16_t> v(kSamples);
    for (size_t i = 0; i < kSamples; ++i) {
        v[i] = static_cast<int16_t>((seq * 100u + i) & 0x7FFF);
    }
    return v;
}

}  // namespace

void test_window_put_get_roundtrip(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(5);
    TEST_ASSERT_TRUE(f.windows.putWindow(5, samples.data(), samples.size()).ok());
    TEST_ASSERT_EQUAL_UINT(1, f.windows.storedCount());
    std::vector<int16_t> out(kSamples, 0);
    auto got = f.windows.getWindow(5, out.data(), out.size());
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(kSamples, got.value);
    TEST_ASSERT_TRUE(samples == out);
}

void test_window_missing_seq_returns_not_found(void) {
    WindowFixture f;
    int16_t out[kSamples];
    auto got = f.windows.getWindow(99, out, kSamples);
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::NotFound), static_cast<uint8_t>(got.err));
}

void test_window_index_rebuilt_across_reboot(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(7);
    TEST_ASSERT_TRUE(f.windows.putWindow(7, samples.data(), samples.size()).ok());
    WindowStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_EQUAL_UINT(1, again.storedCount());
    TEST_ASSERT_TRUE(again.contains(7));
    std::vector<int16_t> out(kSamples, 0);
    TEST_ASSERT_TRUE(again.getWindow(7, out.data(), out.size()).ok());
    TEST_ASSERT_TRUE(samples == out);
}

void test_window_count_limit_evicts_oldest(void) {
    WindowFixture f;
    std::vector<int16_t> out(kSamples, 0);
    for (uint32_t seq = 1; seq <= bw::storage::kMaxStoredWindows; ++seq) {
        const std::vector<int16_t> s = makeSamples(seq);
        TEST_ASSERT_TRUE(f.windows.putWindow(seq, s.data(), s.size()).ok());
    }
    TEST_ASSERT_EQUAL_UINT(bw::storage::kMaxStoredWindows, f.windows.storedCount());
    TEST_ASSERT_EQUAL_UINT32(0, f.counters.evicted_windows);

    const std::vector<int16_t> ninth = makeSamples(99);
    TEST_ASSERT_TRUE(f.windows.putWindow(99, ninth.data(), ninth.size()).ok());
    TEST_ASSERT_EQUAL_UINT(bw::storage::kMaxStoredWindows, f.windows.storedCount());
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.evicted_windows);
    TEST_ASSERT_FALSE(f.windows.contains(1));  // seq 1 was oldest
    TEST_ASSERT_TRUE(f.windows.contains(99));
    // Survivors are still readable.
    TEST_ASSERT_TRUE(f.windows.getWindow(2, out.data(), out.size()).ok());
}

void test_window_reput_same_seq_is_idempotent(void) {
    WindowFixture f;
    for (uint32_t seq = 1; seq <= bw::storage::kMaxStoredWindows; ++seq) {
        const std::vector<int16_t> s = makeSamples(seq);
        TEST_ASSERT_TRUE(f.windows.putWindow(seq, s.data(), s.size()).ok());
    }
    const std::vector<int16_t> again = makeSamples(3);
    TEST_ASSERT_TRUE(f.windows.putWindow(3, again.data(), again.size()).ok());
    TEST_ASSERT_EQUAL_UINT(bw::storage::kMaxStoredWindows, f.windows.storedCount());
    TEST_ASSERT_EQUAL_UINT32(0, f.counters.evicted_windows);  // replace, not evict
}

void test_window_torn_write_discarded_at_mount(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(4);
    // Tear the payload write (op order: header, payload, crc) at byte 50.
    f.fs.armTornWrite(50, 1);
    TEST_ASSERT_FALSE(f.windows.putWindow(4, samples.data(), samples.size()).ok());
    TEST_ASSERT_FALSE(f.windows.contains(4));                     // failed put never indexed
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.torn_tails_repaired);  // runtime repair counted
    TEST_ASSERT_EQUAL_UINT(0, f.fs.fileCount());  // partial blob removed, medium clean

    // A torn blob left BEHIND by a power cut (no chance to clean up) is
    // discarded by the next boot scan, never served (§10).
    const std::vector<uint8_t> partial = {0x46, 0x31, 0x38, 0x57, 4, 0, 0};
    f.fs.setBytes("w_01.bin", partial);
    WindowStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_EQUAL_UINT(0, again.storedCount());
    TEST_ASSERT_TRUE(f.counters.corrupt_records >= 1);
    TEST_ASSERT_FALSE(f.fs.exists("w_01.bin"));
}

void test_window_corrupt_crc_returns_corrupt(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(6);
    TEST_ASSERT_TRUE(f.windows.putWindow(6, samples.data(), samples.size()).ok());
    f.fs.corruptByte("w_00.bin", 40);  // inside the payload, size unchanged
    std::vector<int16_t> out(kSamples, 0);
    auto got = f.windows.getWindow(6, out.data(), out.size());
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Corrupt), static_cast<uint8_t>(got.err));
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.corrupt_records);
}

void test_window_small_buffer_returns_full(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(8);
    TEST_ASSERT_TRUE(f.windows.putWindow(8, samples.data(), samples.size()).ok());
    int16_t out[10];
    auto got = f.windows.getWindow(8, out, 10);
    TEST_ASSERT_FALSE(got.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Full), static_cast<uint8_t>(got.err));
}

void test_window_invalid_args_rejected(void) {
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(1);
    TEST_ASSERT_FALSE(f.windows.putWindow(0, samples.data(), samples.size()).ok());  // seq 0
    TEST_ASSERT_FALSE(f.windows.putWindow(1, nullptr, 10).ok());                     // no data
    TEST_ASSERT_FALSE(f.windows.putWindow(1, samples.data(), 0).ok());               // empty
    const size_t too_many = bw::storage::kRawWindowBytes / 2 + 1;
    std::vector<int16_t> big(too_many, 1);  // > kRawWindowBytes
    TEST_ASSERT_FALSE(f.windows.putWindow(1, big.data(), big.size()).ok());
}

void test_window_fs_full_evicts_then_faults(void) {
    // Capacity fits one ~816 B window but not the next: put #1 succeeds,
    // put #2 (too big to EVER fit) fails its first write, the store drops the
    // oldest OTHER window once and retries, still Full -> honest Err::Full
    // (§10 ladder: drop oldest, retry, then fault upstream).
    WindowFixture f{1024};
    std::vector<int16_t> fits(400, 0);  // 12 + 800 + 4 = 816 B <= 1024
    TEST_ASSERT_TRUE(f.windows.putWindow(1, fits.data(), fits.size()).ok());
    std::vector<int16_t> too_big(600, 7);  // 12 + 1200 + 4 = 1216 B > 1024 alone
    const auto r = f.windows.putWindow(2, too_big.data(), too_big.size());
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Full), static_cast<uint8_t>(r.err));
    TEST_ASSERT_TRUE(f.counters.evicted_windows >= 1);  // oldest dropped first (D6)
    // F-06: the ladder's final refusal is also REPORTED — §10's "then fault
    // upstream" was comment-only before; without this F23 never hears that
    // the window source is down. (Fixture attaches before the puts and the
    // direct fs.mount reports nothing, so the count is purely this refusal.)
    TEST_ASSERT_TRUE(f.faults.countOf(bw::FaultCode::StorageDegraded) >= 1);
}

void test_window_mount_read_failure_keeps_blob(void) {
    // Audit F-04: a failed READ at mount is transient IO, not corruption —
    // the blob must survive it and be served by a later clean boot.
    WindowFixture f;
    const std::vector<int16_t> samples = makeSamples(9);
    TEST_ASSERT_TRUE(f.windows.putWindow(9, samples.data(), samples.size()).ok());
    const uint32_t corrupt_before = f.counters.corrupt_records;

    f.fs.failNextReadFor("w_00.bin");           // transient IO on the mount probe
    WindowStore again{f.fs, f.counters};        // reboot
    TEST_ASSERT_TRUE(again.mount().ok());       // boot never bricks (§10)
    TEST_ASSERT_TRUE(f.fs.exists("w_00.bin"));  // F-04 reverted: deleted!
    TEST_ASSERT_EQUAL_UINT32(corrupt_before + 1, f.counters.corrupt_records);
    TEST_ASSERT_EQUAL_UINT(0, again.storedCount());  // unreadable -> not indexed

    // Clean remount serves the blob in full: the read failure lost nothing.
    WindowStore third{f.fs, f.counters};
    TEST_ASSERT_TRUE(third.mount().ok());
    TEST_ASSERT_TRUE(third.contains(9));
    std::vector<int16_t> out(kSamples, 0);
    TEST_ASSERT_TRUE(third.getWindow(9, out.data(), out.size()).ok());
    TEST_ASSERT_TRUE(samples == out);
}
