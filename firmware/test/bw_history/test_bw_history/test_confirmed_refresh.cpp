// F16 §9 T8: label refresh (spec §6 D8) — setLabel on a finalized day
// inside kRebuildDays refreshes confirmed_count at the next boot(); an
// OLDER day stays stale ON DEVICE, asserted deliberately as the documented
// D8 limitation (the companion recompute from synced events is
// authoritative).

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

// Four logical days, each with one closed episode (seqs 2..5).
uint32_t seedFourDays(bw::testkit::InMemoryEventStore<>& store) {
    (void)appendEvent(store, bootEvent(1, 100));
    for (uint32_t day = 20; day <= 23; ++day) {
        (void)appendEvent(store,
                          sleepClose(wallUnix(day, 10, 0), 1000, 1'800'000, bw::EpisodeClass::Nap));
    }
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(24, 9, 0), 2000));
    return last_seq;
}

}  // namespace

void test_confirmed_refresh_within_rebuild_window(void) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const uint32_t last_seq = seedFourDays(store);

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);

    // Confirm day 23's episode (seq 5) — the NEWEST finalized day, inside
    // kRebuildDays = 2.
    TEST_ASSERT_TRUE(store.setLabel(5, bw::UserLabel::Confirmed).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());

    bh::DailySummary ring[4] = {};
    const size_t n = second.summaries(ring, 4);
    TEST_ASSERT_EQUAL_UINT32(4, n);  // days 20..23 finalized; day 24 open
    TEST_ASSERT_EQUAL_UINT32(23, ring[3].day_key);
    TEST_ASSERT_EQUAL_UINT8(1, ring[3].confirmed_count);  // refreshed at boot()
}

void test_confirmed_refresh_older_day_stays_stale(void) {
    // D8: the rebuild covers only the newest kRebuildDays day records.
    // Confirming day 21's episode (seq 3) does NOT reach the device record
    // after a reboot — pinned here so the limitation stays visible; the
    // companion recompute (with labels) is authoritative for old days.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const uint32_t last_seq = seedFourDays(store);

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);

    TEST_ASSERT_TRUE(store.setLabel(3, bw::UserLabel::Confirmed).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());

    bh::DailySummary ring[4] = {};
    const size_t n = second.summaries(ring, 4);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    // Day 21 = ring[1]; newest two day records are 22 and 23 -> stale here.
    TEST_ASSERT_EQUAL_UINT32(21, ring[1].day_key);
    TEST_ASSERT_EQUAL_UINT8(0, ring[1].confirmed_count);
}

void test_confirmed_refresh_live_day_not_lost_by_reboot(void) {
    // A confirmation inside the CURRENT (still open) day is re-derived at
    // the reboot: the open day replays from its first event with the new
    // label (D3 + D8 together).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    const uint32_t seq_live =
        appendEvent(store, sleepClose(wallUnix(20, 10, 0), 1000, 1'800'000, bw::EpisodeClass::Nap));

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, seq_live);  // day 20 stays OPEN (no closing event yet)
    TEST_ASSERT_EQUAL_UINT32(0, first.summaryCount());

    TEST_ASSERT_TRUE(store.setLabel(seq_live, bw::UserLabel::Confirmed).ok());

    // The closing event arrives only AFTER the first drain — if it were
    // already in the log, one step() would drain it in the same batch and
    // finalize day 20 in the first boot (batching defeats the fixture).
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(21, 9, 0), 2000));

    // Reboot with the day still open; the closing event arrives afterwards.
    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, seq_end);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, second.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(20, ring[0].day_key);
    TEST_ASSERT_EQUAL_UINT8(1, ring[0].confirmed_count);  // replay applied it
    TEST_ASSERT_EQUAL_UINT32(seq_live, ring[0].first_seq);
}
