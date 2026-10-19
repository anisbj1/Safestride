// F16 §9 T5 (DoD §14): summaries must be reproducible from raw events
// alone. A multi-day fixture runs incrementally (tiny ticks), then every
// finalized day is recomputed independently from IEventStore.readFrom over
// its first_seq..last_seq range and compared BYTE-FOR-BYTE — against both
// the in-RAM ring snapshot and the persisted KV blob. The second pass adds
// a setLabel + reboot so the D8 rebuild path is compared the same way
// (within kRebuildDays; older staleness is T8's documented limitation).

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

// Three logical days of mixed events, all with real wall time.
uint32_t seedReproFixture(bw::testkit::InMemoryEventStore<>& store) {
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 9, 30), 1000));
    (void)appendEvent(
        store, sleepClose(wallUnix(5, 23, 40), 2000, 25'200'000, bw::EpisodeClass::MainSleep));
    (void)appendEvent(store,
                      sleepClose(wallUnix(6, 10, 0), 3000, 1'800'000, bw::EpisodeClass::Rest));
    (void)appendEvent(store, sleepOpenMarker(wallUnix(6, 12, 0), 4000));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 15, 0), 5000));
    (void)appendEvent(store, sleepRecovered(wallUnix(6, 23, 0), 6000));
    (void)appendEvent(store, napCompleted(1, wallUnix(7, 8, 0), 7000));
    (void)appendEvent(store,
                      sleepClose(wallUnix(7, 13, 0), 8000, 2'700'000, bw::EpisodeClass::Nap));
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(8, 9, 0), 9000));
    return last_seq;
}

// The DoD check: a full recompute of the record's own first_seq..last_seq
// range must equal the incremental record byte-for-byte.
void assertAgainstFullRecompute(bw::IEventStore& store, const bh::DailySummary& record) {
    const bh::DailySummary fresh =
        recomputeDay(store, record.first_seq, record.last_seq, record.day_key);
    TEST_ASSERT_EQUAL_MEMORY(&fresh, &record, sizeof(bh::DailySummary));
}

// The "stored record" half of the DoD: the KV blob at slot_index must hold
// exactly the same bytes as the ring record. Only valid while no boot-time
// rebuild has advanced RAM past the last persisted finalize.
void assertKvBlobMatches(bw::IKeyValueStore& kv, const bh::DailySummary& record,
                         size_t slot_index) {
    uint8_t blob[bh::kSummaryBlobBytes] = {};
    char key[bh::kKvDayKeyBytes] = {};
    bh::dayKvKey(key, sizeof(key), static_cast<uint8_t>(slot_index));
    TEST_ASSERT_TRUE(kv.get(key, blob, sizeof(blob)).ok());
    bh::DailySummary stored{};
    TEST_ASSERT_TRUE(bh::decodeSummaryBlob(blob, bh::kSummaryBlobBytes, stored));
    TEST_ASSERT_EQUAL_MEMORY(&record, &stored, sizeof(bh::DailySummary));
}

}  // namespace

void test_reproducibility_incremental_vs_full_recompute(void) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const uint32_t last_seq = seedReproFixture(store);

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);  // incremental: each step drains <= 8 events

    bh::DailySummary ring[bh::kDeviceSummaryDays] = {};
    const size_t n = agg.summaries(ring, bh::kDeviceSummaryDays);
    TEST_ASSERT_EQUAL_UINT32(3, n);  // days 5-7 finalized; day 8 open
    for (size_t i = 0; i < n; ++i) {
        assertAgainstFullRecompute(store, ring[i]);
        assertKvBlobMatches(kv, ring[i], i);
    }
}

void test_reproducibility_after_label_change_and_reboot(void) {
    // F14 confirms episodes AFTER their days were finalized; the D8 rebuild
    // at the next boot must produce exactly what a full recompute with the
    // new labels produces — byte-for-byte (T5's second half).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const uint32_t last_seq = seedReproFixture(store);

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);

    // Confirm day 5's MainSleep (seq 3) and day 6's Rest episode (seq 4).
    TEST_ASSERT_TRUE(store.setLabel(3, bw::UserLabel::Confirmed).ok());
    TEST_ASSERT_TRUE(store.setLabel(4, bw::UserLabel::Confirmed).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, last_seq);

    bh::DailySummary ring[bh::kDeviceSummaryDays] = {};
    const size_t n = second.summaries(ring, bh::kDeviceSummaryDays);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    // Day 6 is inside kRebuildDays = 2: its confirmed count refreshed and
    // the whole record matches a full recompute with the new label.
    TEST_ASSERT_EQUAL_UINT8(1, ring[1].confirmed_count);
    assertAgainstFullRecompute(store, ring[1]);
    // Day 7 had no label change: rebuilt identically.
    assertAgainstFullRecompute(store, ring[2]);
    // Day 5 is OUTSIDE the rebuild window: the documented D8 limitation —
    // stale until the companion recompute (asserted deliberately, T8 too).
    TEST_ASSERT_EQUAL_UINT8(0, ring[0].confirmed_count);
}

void test_reproducibility_recovered_episode_is_zero_ms(void) {
    // §6 D7: a recovered episode counts as Uncertain with 0 ms — the
    // recompute must agree on every field, including the honest zeros.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    const uint32_t seq_rec = appendEvent(store, sleepRecovered(wallUnix(5, 23, 0), 2000));
    (void)appendEvent(store,
                      sleepClose(wallUnix(5, 10, 0), 3000, 1'800'000, bw::EpisodeClass::Nap));
    const uint32_t last_seq = appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 4000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, last_seq);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT8(2, ring[0].episode_count);
    TEST_ASSERT_EQUAL_UINT32(1'800'000, ring[0].total_sleep_ms);  // recovered adds 0
    TEST_ASSERT_EQUAL_UINT32(seq_rec, ring[0].first_seq);
    assertAgainstFullRecompute(store, ring[0]);
    assertKvBlobMatches(kv, ring[0], 0);
}
