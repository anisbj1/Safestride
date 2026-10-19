// F16 §9 T1: basic aggregation from event fixtures — totals, daytime/main
// shares, counts, first/last_seq, ring order (spec §6 D5–D7).

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

}  // namespace

void test_aggregation_basic(void) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    // Logical day 5 (wall 04:00 day 5 -> 04:00 day 6): a Boot anchor, a nap
    // at 20:00, a night MainSleep starting 23:50 (7 h 20 min — crosses the
    // 04:00 boundary, attributed WHOLE to its start day, D5), a Rest episode
    // at 10:00 (daytime per D6), an open marker (skipped) and an ignored
    // type. A day-6 nap finalizes day 5.
    (void)appendEvent(store, bootEvent(1, 100));
    const uint32_t seq_nap = appendEvent(store, napCompleted(1, wallUnix(5, 20, 0), 1000));
    (void)appendEvent(
        store, sleepClose(wallUnix(5, 23, 50), 2000, 26'400'000, bw::EpisodeClass::MainSleep));
    const uint32_t seq_rest =
        appendEvent(store, sleepClose(wallUnix(5, 10, 0), 3000, 1'800'000, bw::EpisodeClass::Rest));
    (void)appendEvent(store, sleepOpenMarker(wallUnix(5, 11, 0), 4000));
    (void)appendEvent(store, ignoredDrowsy(wallUnix(5, 12, 0), 5000));
    const uint32_t seq_nap2 = appendEvent(store, napCompleted(1, wallUnix(6, 8, 0), 6000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_nap2);

    // Day 5 finalized + day 6 open (its own record waits for day 7) — the
    // ring snapshot is oldest -> newest.
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));

    bh::DailySummary expected{};
    expected.day_key = 5;
    expected.boot_id = 0;  // wall time known: calendar day, not a session
    expected.first_seq = seq_nap;
    expected.last_seq = seq_rest;
    expected.total_sleep_ms = 26'400'000 + 1'800'000;
    expected.daytime_sleep_ms = 1'800'000;  // only the 10:00 Rest episode (D6)
    expected.main_sleep_ms = 26'400'000;
    expected.flags = 0;
    expected.episode_count = 2;  // open marker and DrowsinessEvent excluded
    expected.nap_count = 1;
    expected.confirmed_count = 0;
    TEST_ASSERT_EQUAL_MEMORY(&expected, &ring[0], sizeof(expected));

    // The latest record accessor returns the newest FINALIZED day.
    bh::DailySummary latest{};
    TEST_ASSERT_TRUE(agg.latestDailySummary(latest));
    TEST_ASSERT_EQUAL_UINT32(5, latest.day_key);
}

void test_aggregation_empty_store_is_honest(void) {
    // §10: an empty log yields NO slot at all — never a zeroed fabricated
    // summary (00 §1).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    TEST_ASSERT_TRUE(agg.step().ok());
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());
    bh::DailySummary out{};
    TEST_ASSERT_FALSE(agg.latestDailySummary(out));
}

void test_foreign_detector_episode_not_counted(void) {
    // §6 D7 + audit finding 8: the payload mirror is F08's layout
    // (detector 8) — a closed SleepEpisode from any OTHER detector must be
    // skipped entirely (not counted, not anchoring first_seq), so a future
    // second producer can never be double-counted with mis-decoded bytes.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    // Foreign producer FIRST (detector 7), then F08's own episode (8).
    (void)appendEvent(store,
                      sleepClose(wallUnix(5, 8, 0), 1000, 7'200'000, bw::EpisodeClass::MainSleep, 1,
                                 bw::UserLabel::Unlabeled, 0, /*detector_id=*/7));
    const uint32_t seq_own =
        appendEvent(store, sleepClose(wallUnix(5, 9, 0), 2000, 1'800'000, bw::EpisodeClass::Nap));
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(6, 8, 0), 3000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT8(1, ring[0].episode_count);            // pre-fix: 2
    TEST_ASSERT_EQUAL_UINT32(seq_own, ring[0].first_seq);         // pre-fix: foreign seq
    TEST_ASSERT_EQUAL_UINT32(1'800'000, ring[0].total_sleep_ms);  // own episode only
    TEST_ASSERT_EQUAL_UINT8(
        0, ring[0].nap_count);  // NapCompleted (day 6) accrues to its own, still-open day
}
