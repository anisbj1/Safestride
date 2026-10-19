// F16 §9 T6: clock regression and forward jumps (spec §6 D9, §10) — a
// backward day-key jump finalizes and reopens (two slots may share a key)
// and bumps clock_regressions; a forward jump finalizes as seen with no
// synthetic empty days.

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

}  // namespace

void test_clock_regression_backward_jump_reopens_and_counts(void) {
    // Day 5 -> day 4 (backward) -> day 5 again: three records, the two day-5
    // slots distinguished by first_seq, and exactly one regression counted
    // (persisted in f16.meta for the next boot).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    const uint32_t seq_a = appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));
    const uint32_t seq_b = appendEvent(store, napCompleted(1, wallUnix(4, 10, 0), 2000));
    const uint32_t seq_c = appendEvent(store, napCompleted(1, wallUnix(5, 12, 0), 3000));
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 4000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(3, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(5, ring[0].day_key);
    TEST_ASSERT_EQUAL_UINT32(seq_a, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT32(4, ring[1].day_key);  // the regression target
    TEST_ASSERT_EQUAL_UINT32(seq_b, ring[1].first_seq);
    TEST_ASSERT_EQUAL_UINT32(5, ring[2].day_key);  // second slot, same key
    TEST_ASSERT_EQUAL_UINT32(seq_c, ring[2].first_seq);
    TEST_ASSERT_EQUAL_UINT32(1, agg.clockRegressions());
    TEST_ASSERT_EQUAL_UINT8(1, ring[2].nap_count);  // no data lost, no merge

    // The counter survives the next boot via f16.meta.
    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    TEST_ASSERT_EQUAL_UINT32(1, second.clockRegressions());
}

void test_clock_regression_forward_jump_no_synthetic_days(void) {
    // Day 3 -> day 9: intermediate days are NOT fabricated (§10: empty day
    // yields no slot at all); no regression is counted.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(3, 10, 0), 1000));
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(9, 10, 0), 2000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(3, ring[0].day_key);  // day 9 is still open
    TEST_ASSERT_EQUAL_UINT32(0, agg.clockRegressions());

    // One more forward jump finalizes day 9 as seen — still no fillers.
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(12, 10, 0), 3000));
    drainTo(agg, seq_end);
    TEST_ASSERT_EQUAL_UINT32(2, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(9, ring[1].day_key);
    TEST_ASSERT_EQUAL_UINT32(0, agg.clockRegressions());
}

void test_clock_regression_session_day_interleaves_cleanly(void) {
    // A session summary (unknown wall time) between two days is not a
    // regression: only backward DAY-key jumps count (§6 D9).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 10, 0), 1000));
    // Clock lost (unix 0) but SAME boot: derived? no — clock is unsynced
    // here (FakeClock starts at 0), so this goes to a session summary only
    // if its boot differs... same boot + unsynced clock = session too (D4
    // fallback: no wall time available at all).
    const uint32_t seq_sess = appendEvent(store, napCompleted(1, 0, 2000));
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(7, 10, 0), 3000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(2, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(6, ring[0].day_key);
    TEST_ASSERT_EQUAL_UINT32(0, ring[1].day_key);  // session
    TEST_ASSERT_EQUAL_UINT32(seq_sess, ring[1].first_seq);
    TEST_ASSERT_EQUAL_UINT32(0, agg.clockRegressions());  // 7 > 6: not backward
}

void test_clock_regression_persisted_on_last_finalize(void) {
    // Audit A4: the regression happens on the LAST tick before power-down —
    // no later finalize exists to carry the counter into f16.meta. The
    // increment must land through the same meta write path immediately.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 10, 0), 1000));  // opens day 6
    const uint32_t last_seq =
        appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 2000));  // backward jump

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);
    TEST_ASSERT_EQUAL_UINT32(1, agg.clockRegressions());  // in RAM
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());      // day 6 finalized

    // Power-down RIGHT HERE — no further event, no further finalize. A
    // fresh instance must still see the counter (pre-fix it read 0: the
    // increment happened AFTER the last meta write).
    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    TEST_ASSERT_EQUAL_UINT32(1, second.clockRegressions());
}
