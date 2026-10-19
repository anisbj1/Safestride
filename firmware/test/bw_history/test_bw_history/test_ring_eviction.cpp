// F16 §9 T7: ring eviction (spec §8 kDeviceSummaryDays = 7, §6 D1) — the
// oldest record is evicted beyond capacity and summaries() snapshots the
// ring oldest -> newest.

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

}  // namespace

void test_ring_eviction_keeps_newest_seven(void) {
    // Nine logical days of naps: days 10..18. Finalization happens when the
    // NEXT day opens, so day 18 is finalized by day 19's first event.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    for (uint32_t day = 10; day <= 18; ++day) {
        (void)appendEvent(store, napCompleted(1, wallUnix(day, 10, 0), 1000 + day));
    }
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(19, 10, 0), 2000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);

    // Days 10..18 finalized; the ring holds only the newest 7: 12..18.
    TEST_ASSERT_EQUAL_UINT32(bh::kDeviceSummaryDays, agg.summaryCount());
    bh::DailySummary ring[bh::kDeviceSummaryDays + 1] = {};
    TEST_ASSERT_EQUAL_UINT32(bh::kDeviceSummaryDays,
                             agg.summaries(ring, bh::kDeviceSummaryDays + 1));
    for (size_t i = 0; i < bh::kDeviceSummaryDays; ++i) {
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(12 + i), ring[i].day_key);
        TEST_ASSERT_EQUAL_UINT8(1, ring[i].nap_count);
    }

    // latestDailySummary is the newest record.
    bh::DailySummary latest{};
    TEST_ASSERT_TRUE(agg.latestDailySummary(latest));
    TEST_ASSERT_EQUAL_UINT32(18, latest.day_key);

    // The eviction also survives a reboot (meta count + slot order).
    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    bh::DailySummary reloaded[bh::kDeviceSummaryDays] = {};
    TEST_ASSERT_EQUAL_UINT32(bh::kDeviceSummaryDays,
                             second.summaries(reloaded, bh::kDeviceSummaryDays));
    for (size_t i = 0; i < bh::kDeviceSummaryDays; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&ring[i], &reloaded[i], sizeof(bh::DailySummary));
    }
}

void test_ring_eviction_session_records_occupy_slots(void) {
    // Session summaries are ordinary ring citizens: they occupy a slot and
    // age out exactly like day records (§6 D4(a)).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    // 8 session days (boot 2..9), each with one nap, plus a final day event
    // to trigger the last finalization: only the newest 7 records remain.
    uint32_t last_seq = 0;
    for (uint32_t boot = 2; boot <= 9; ++boot) {
        (void)appendEvent(store, bootEvent(boot, 100 * boot));
        last_seq = appendEvent(store, napCompleted(boot, 0, 200 * boot));
    }
    const uint32_t seq_end = appendEvent(store, napCompleted(9, wallUnix(30, 10, 0), 3000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    bh::DailySummary ring[bh::kDeviceSummaryDays] = {};
    const size_t n = agg.summaries(ring, bh::kDeviceSummaryDays);
    TEST_ASSERT_EQUAL_UINT32(bh::kDeviceSummaryDays, n);
    // The 8 sessions finalized as boots 3..9 arrived; boot 2's aged out, so
    // the ring holds sessions 3..9 oldest -> newest (day 30 is still open).
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_UINT32(0, ring[i].day_key);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(3 + i), ring[i].boot_id);
    }
    (void)last_seq;
}

void test_full_ring_crash_window_replay_no_duplicate(void) {
    // Audit finding 4: the crash-window dedupe branch (persistSlots landed,
    // persistMeta failed, ring FULL) is load-bearing — power loss in that
    // window leaves a day in a slot that meta does not vouch for. On reboot
    // the replay re-opens that day; finalizing it must REPLACE the newest
    // slot (same identity + first_seq), not evict + append a duplicate.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> mem;
    bw::testkit::InMemoryKeyValueStore<> memkv;
    FaultyKvStore kv(memkv);

    (void)appendEvent(mem, bootEvent(1, 100));  // identity anchor: without it no day derives
    // Days 1..8 first; day 9's trigger event is appended only after the ring
    // is full, otherwise batch 2 swallows it and day 8 finalizes with a
    // healthy meta (no crash window to inject).
    for (uint32_t day = 1; day <= 8; ++day) {
        (void)appendEvent(mem, napCompleted(1, wallUnix(day, 10, 0), 1000 + day));
    }

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &mem, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    // Batch 1 (Boot + days 1..7): days 1..6 finalized, day 7 open.
    TEST_ASSERT_TRUE(agg.step().ok());
    TEST_ASSERT_EQUAL_UINT32(6, agg.summaryCount());
    // Batch 2 (day 8): day 7 finalizes → ring FULL (7), day 8 open.
    TEST_ASSERT_TRUE(agg.step().ok());
    TEST_ASSERT_EQUAL_UINT32(bh::kDeviceSummaryDays, agg.summaryCount());

    const uint32_t seq_end = appendEvent(mem, napCompleted(1, wallUnix(9, 10, 0), 2000));
    // Crash window: slots land (disk now holds days 2..8), meta put fails.
    kv.failPutFor(bh::kKvKeyMeta);
    const bw::Result<void> r = agg.step();  // day 9 arrives → finalize day 8
    TEST_ASSERT_FALSE(r.ok());
    kv.failPutFor(nullptr);
    bh::DailySummary ram[8] = {};
    TEST_ASSERT_EQUAL_UINT32(7, agg.summaries(ram, 8));  // RAM untouched
    TEST_ASSERT_EQUAL_UINT32(1, ram[0].day_key);         // still days 1..7

    // Reboot: meta vouches for 7 slots (cursor = day 8's trigger point) but
    // the SLOTS on disk are one ahead — they already contain day 8.
    bh::HistoryAggregator second{bh::HistoryConfig{}, &mem, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    bh::DailySummary pre[8] = {};
    TEST_ASSERT_EQUAL_UINT32(7, second.summaries(pre, 8));
    TEST_ASSERT_EQUAL_UINT32(2, pre[0].day_key);  // slots ahead of meta

    // Replay re-opens day 8; day 9 finalizes it → replace, not duplicate.
    drainTo(second, seq_end);
    bh::DailySummary ring[8] = {};
    TEST_ASSERT_EQUAL_UINT32(7, second.summaries(ring, 8));
    for (size_t i = 0; i < 7; ++i) {
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(2 + i), ring[i].day_key);
        TEST_ASSERT_EQUAL_UINT8(1, ring[i].nap_count);
    }
    // No two slots may share identity + first_seq (the branch's contract).
    for (size_t i = 0; i < 7; ++i) {
        for (size_t j = i + 1; j < 7; ++j) {
            TEST_ASSERT_FALSE(ring[i].day_key == ring[j].day_key &&
                              ring[i].first_seq == ring[j].first_seq);
        }
    }
}
