// F16 §9 T10: incremental batching — a backlog larger than
// kAggMaxEventsPerTick drains over multiple ticks with a monotonically
// advancing cursor and lands on exactly the one-shot recompute (spec §6 D3).

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

}  // namespace

void test_incremental_batching_backlog_matches_oneshot(void) {
    // 20 counting events over 3 logical days > 8 events/tick: at least
    // three ticks are needed to drain.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    for (int i = 0; i < 7; ++i) {
        (void)appendEvent(store, napCompleted(1, wallUnix(30, 9, 0), 1000 + i));
    }
    for (int i = 0; i < 7; ++i) {
        (void)appendEvent(store, napCompleted(1, wallUnix(31, 9, 0), 2000 + i));
    }
    for (int i = 0; i < 6; ++i) {
        (void)appendEvent(store, napCompleted(1, wallUnix(32, 9, 0), 3000 + i));
    }
    const uint32_t last_seq = 21;
    TEST_ASSERT_EQUAL_UINT32(last_seq, store.nextSeq() - 1);

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());

    // Cursor must advance monotonically across the draining ticks.
    uint32_t prev_cursor = agg.lastProcessedSeq();
    size_t ticks_used = 0;
    for (size_t i = 0; i < 16 && agg.lastProcessedSeq() < last_seq; ++i) {
        TEST_ASSERT_TRUE(agg.step().ok());
        const uint32_t cursor = agg.lastProcessedSeq();
        TEST_ASSERT_TRUE(cursor >= prev_cursor);
        prev_cursor = cursor;
        ++ticks_used;
    }
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg.lastProcessedSeq());
    TEST_ASSERT_TRUE(ticks_used >= 3);  // 20 events > 8/tick: no one-shot drain

    // The batched result equals the one-shot recompute per day.
    bh::DailySummary ring[4] = {};
    const size_t n = agg.summaries(ring, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n);  // days 30, 31 finalized; day 32 open
    const bh::DailySummary day30 = recomputeDay(store, 2, 8, 30);
    const bh::DailySummary day31 = recomputeDay(store, 9, 15, 31);
    TEST_ASSERT_EQUAL_MEMORY(&day30, &ring[0], sizeof(bh::DailySummary));
    TEST_ASSERT_EQUAL_MEMORY(&day31, &ring[1], sizeof(bh::DailySummary));
    TEST_ASSERT_EQUAL_UINT8(7, ring[0].nap_count);
    TEST_ASSERT_EQUAL_UINT8(7, ring[1].nap_count);
}

void test_incremental_batching_batch_size_is_bounded(void) {
    // §8 kAggMaxEventsPerTick: a single tick never consumes more than the
    // named batch (the §11 RAM budget depends on it).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    for (int i = 0; i < 10; ++i) {
        (void)appendEvent(store, napCompleted(1, wallUnix(30, 9, 0), 1000 + i));
    }

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    TEST_ASSERT_TRUE(agg.step().ok());
    // First tick: boot event + at most kAggMaxEventsPerTick events — the
    // cursor lands exactly on 1 + kAggMaxEventsPerTick when more are due.
    TEST_ASSERT_TRUE(agg.lastProcessedSeq() <= 1 + bh::kAggMaxEventsPerTick);
    TEST_ASSERT_TRUE(agg.lastProcessedSeq() >= bh::kAggMaxEventsPerTick);
}
