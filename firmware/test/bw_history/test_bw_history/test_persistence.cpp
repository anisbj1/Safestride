// F16 §9 T4 + T11: cursor persistence across simulated reboots, empty and
// single-event stores, provider read errors (§10) and corrupt/unknown-schema
// KV blobs (§10: discard + full rescan, always correct because summaries
// are event-derived — DoD §14).

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

// Two logical days of naps: day 5 (finalized when day 6 opens) and day 6
// (still open at the end).
struct TwoDayFixture {
    uint32_t last_seq = 0;
};

TwoDayFixture seedTwoDays(bw::testkit::InMemoryEventStore<>& store) {
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 22, 0), 2000));
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 3000));
    return TwoDayFixture{seq_end};
}

}  // namespace

void test_cursor_persisted_across_reboot_no_double_count(void) {
    // T4: a simulated reboot (new aggregator, same KV) restores the cursor:
    // no day is re-counted and the open day rebuilds to the same bytes.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const TwoDayFixture fx = seedTwoDays(store);

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, fx.last_seq);
    bh::DailySummary before[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, first.summaries(before, 4));

    // Reboot: new instance over the same KV + event log.
    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, fx.last_seq);
    bh::DailySummary after[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, second.summaries(after, 4));
    TEST_ASSERT_EQUAL_MEMORY(&before[0], &after[0], sizeof(bh::DailySummary));
    // Day 5's nap_count would be 2 if the two events were double-counted.
    TEST_ASSERT_EQUAL_UINT8(2, after[0].nap_count);
}

void test_cursor_persisted_single_event_stays_open(void) {
    // T4: exactly one counting event — no later day ever finalizes it, so
    // the ring is honestly empty (§10: no fabricated zeroed summary) and a
    // reboot re-derives the same open day when its closing event arrives.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(1, 100));
    const uint32_t seq_nap = appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, seq_nap);
    TEST_ASSERT_EQUAL_UINT32(0, first.summaryCount());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, seq_nap);
    TEST_ASSERT_EQUAL_UINT32(0, second.summaryCount());

    // The closing event arrives after the "reboot": the day materializes
    // exactly once (cursor replay is deduplicated by construction).
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(6, 8, 0), 2000));
    drainTo(second, seq_end);
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, second.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(5, ring[0].day_key);
    TEST_ASSERT_EQUAL_UINT8(1, ring[0].nap_count);
    TEST_ASSERT_EQUAL_UINT32(seq_nap, ring[0].first_seq);
}

void test_read_error_skips_tick_and_retries(void) {
    // T4 / §10: a readFrom failure leaves the aggregator state unchanged
    // (cursor, summaries) and the next healthy tick processes normally.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> mem;
    bw::testkit::InMemoryKeyValueStore<> kv;
    const TwoDayFixture fx = seedTwoDays(mem);
    FaultyEventStore<64> store(mem);

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());

    store.failReads(true);
    const bw::Result<void> r = agg.step();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::BusError),
                             static_cast<uint32_t>(r.err));
    TEST_ASSERT_EQUAL_UINT32(0, agg.lastProcessedSeq());  // state unchanged
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());

    store.failReads(false);
    drainTo(agg, fx.last_seq);
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT8(2, ring[0].nap_count);  // no double counting
}

void test_kv_corrupt_slot_discard_and_full_rescan(void) {
    // T11: a corrupt blob in one day slot discards the persisted state; the
    // full rescan regenerates EVERY day byte-for-byte (valid slots survive
    // content-wise because summaries are event-derived).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));
    (void)appendEvent(store,
                      sleepClose(wallUnix(5, 22, 0), 2000, 3'600'000, bw::EpisodeClass::MainSleep));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 3000));
    (void)appendEvent(store, napCompleted(1, wallUnix(7, 9, 0), 4000));
    const uint32_t last_seq = 5;

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);
    bh::DailySummary before[4] = {};
    const size_t n_before = first.summaries(before, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n_before);

    // Corrupt slot 0 with a wrong-schema blob (same size as a real one).
    uint8_t garbage[bh::kSummaryBlobBytes];
    std::memset(garbage, 0xAB, sizeof(garbage));
    TEST_ASSERT_TRUE(kv.put("f16.day.0", garbage, sizeof(garbage)).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, last_seq);
    bh::DailySummary after[4] = {};
    TEST_ASSERT_EQUAL_UINT32(n_before, second.summaries(after, 4));
    for (size_t i = 0; i < n_before; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after[i], sizeof(bh::DailySummary));
    }
}

void test_kv_corrupt_meta_discard_and_full_rescan(void) {
    // T11: corrupt f16.meta ⇒ same result as a clean full recompute.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 2000));
    (void)appendEvent(store, napCompleted(1, wallUnix(7, 9, 0), 3000));
    const uint32_t last_seq = 4;

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);
    bh::DailySummary before[4] = {};
    const size_t n_before = first.summaries(before, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n_before);

    uint8_t garbage[bh::kMetaBlobBytes];
    std::memset(garbage, 0xCD, sizeof(garbage));
    TEST_ASSERT_TRUE(kv.put(bh::kKvKeyMeta, garbage, sizeof(garbage)).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, last_seq);
    bh::DailySummary after[4] = {};
    TEST_ASSERT_EQUAL_UINT32(n_before, second.summaries(after, 4));
    for (size_t i = 0; i < n_before; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after[i], sizeof(bh::DailySummary));
    }
}

void test_kv_unknown_meta_schema_discard(void) {
    // T11: right size, unknown kMetaSchemaVersion ⇒ discarded (§10).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, wallUnix(5, 10, 0), 1000));
    (void)appendEvent(store, napCompleted(1, wallUnix(6, 9, 0), 2000));
    const uint32_t last_seq = 3;

    bh::HistoryAggregator first{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, last_seq);
    TEST_ASSERT_EQUAL_UINT32(1, first.summaryCount());

    uint8_t future[bh::kMetaBlobBytes] = {};
    future[0] = 99;  // unknown schema
    TEST_ASSERT_TRUE(kv.put(bh::kKvKeyMeta, future, sizeof(future)).ok());

    bh::HistoryAggregator second{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    drainTo(second, last_seq);
    bh::DailySummary after[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, second.summaries(after, 4));
    TEST_ASSERT_EQUAL_UINT32(5, after[0].day_key);
    TEST_ASSERT_EQUAL_UINT8(1, after[0].nap_count);
}

void test_kv_read_error_fails_boot_without_discarding(void) {
    // §10 + audit A1: a KV READ ERROR is a distinct, retried failure — NOT
    // the corrupt-state discard. A discarding boot would wipe a wall-time-
    // derived day that the rescan cannot regenerate (that boot's clock
    // mapping is gone), so boot() must return Err with booted_ unset, the
    // next tick must retry the same failure, and after the store heals the
    // original record must still be there byte for byte.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> mem;
    bw::testkit::InMemoryKeyValueStore<> memkv;

    // Boot 7: a unix-0 SleepEpisode clock-derived into day 7 (D4) + a day-8
    // nap that finalizes day 7 with kFlagWallTimeDerived set.
    clock.setUnixSeconds(wallUnix(7, 12, 0));
    clock.setMs(10'000'000);
    (void)appendEvent(mem, bootEvent(7, 100));
    (void)appendEvent(mem,
                      sleepClose(0, 10'000'000 - 3'600'000, 1'800'000, bw::EpisodeClass::Nap, 7));
    const uint32_t seq_end = appendEvent(mem, napCompleted(7, wallUnix(8, 8, 0), 11'000'000));

    bh::HistoryAggregator first{bh::HistoryConfig{}, &mem, &memkv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(first).ok());
    drainTo(first, seq_end);
    bh::DailySummary before[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, first.summaries(before, 4));
    TEST_ASSERT_TRUE((before[0].flags & bh::kFlagWallTimeDerived) != 0);

    // Reboot with failing KV READS: boot() must report the error (pre-fix
    // it returned Ok after silently discarding — audit A1).
    FaultyKvStore faulty(memkv);
    faulty.failReads(true);
    bh::HistoryAggregator second{bh::HistoryConfig{}, &mem, &faulty, &clock};
    const bw::Result<void> b = f16_test::bootToReady(second);
    TEST_ASSERT_FALSE(b.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::BusError),
                             static_cast<uint32_t>(b.err));
    // Not ready: the same restore phase retries and reports the error rather
    // than discarding or rescanning over the persisted record.
    const bw::Result<void> t = second.step();
    TEST_ASSERT_FALSE(t.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::BusError),
                             static_cast<uint32_t>(t.err));

    // Store heals → bounded retries resume the exact restore phase and LOAD
    // the original record unchanged: nothing was discarded, no silent loss.
    faulty.failReads(false);
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    bh::DailySummary after[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, second.summaries(after, 4));
    TEST_ASSERT_EQUAL_MEMORY(&before[0], &after[0], sizeof(bh::DailySummary));
    TEST_ASSERT_EQUAL_UINT32(7, after[0].day_key);
    TEST_ASSERT_TRUE((after[0].flags & bh::kFlagWallTimeDerived) != 0);
}

void test_kv_slot_write_failure_retries_finalize(void) {
    // §10 write-failure path 1 (audit finding 3a): persistSlots' put fails →
    // step() returns Err, RAM is untouched (count + cursor), the triggering
    // event stays unconsumed, and the next healthy tick finalizes the day
    // exactly once.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> mem;
    bw::testkit::InMemoryKeyValueStore<> memkv;
    const TwoDayFixture fx = seedTwoDays(mem);
    FaultyKvStore kv(memkv);

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &mem, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());

    kv.failPuts(true);
    const bw::Result<void> r = agg.step();  // day-6 event triggers the finalize
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::BusError),
                             static_cast<uint32_t>(r.err));
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());      // RAM untouched
    TEST_ASSERT_EQUAL_UINT32(3, agg.lastProcessedSeq());  // day-6 event not consumed

    kv.failPuts(false);
    drainTo(agg, fx.last_seq);
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(5, ring[0].day_key);
    TEST_ASSERT_EQUAL_UINT8(2, ring[0].nap_count);  // both naps, exactly once
    TEST_ASSERT_EQUAL_UINT32(2, ring[0].first_seq);
}

void test_kv_meta_write_failure_lands_day_once(void) {
    // §10 write-failure path 2 (audit finding 3b): the slots land but
    // f16.meta's put fails → open_valid_ restored, RAM unchanged, event
    // unconsumed; the retry rewrites the same slots and lands the day
    // exactly once — no duplicate in RAM, on disk, or after a reboot.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> mem;
    bw::testkit::InMemoryKeyValueStore<> memkv;
    FaultyKvStore kv(memkv);

    (void)appendEvent(mem, bootEvent(1, 100));
    (void)appendEvent(mem, napCompleted(1, wallUnix(4, 10, 0), 1000));
    const uint32_t seq_d5 = appendEvent(mem, napCompleted(1, wallUnix(5, 10, 0), 2000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &mem, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_d5);  // day 4 finalized with a clean meta; day 5 open
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());

    // The day-6 event must arrive only now: if it exists before the first
    // drain, the batching step swallows it in the same batch and day 5
    // finalizes early with a healthy meta (no fault to inject).
    const uint32_t seq_end = appendEvent(mem, napCompleted(1, wallUnix(6, 9, 0), 3000));
    kv.failPutFor(bh::kKvKeyMeta);
    const bw::Result<void> r = agg.step();  // day 6 arrives → finalize day 5
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::BusError),
                             static_cast<uint32_t>(r.err));
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());           // day 5 not in RAM yet
    TEST_ASSERT_EQUAL_UINT32(seq_d5, agg.lastProcessedSeq());  // event not consumed

    kv.failPutFor(nullptr);
    drainTo(agg, seq_end);
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(2, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(5, ring[1].day_key);  // landed exactly once
    TEST_ASSERT_EQUAL_UINT32(seq_d5, ring[1].first_seq);

    // The disk state right after the failed meta put (slot written, meta not)
    // replays without duplicating day 5 after a reboot either.
    bh::HistoryAggregator second{bh::HistoryConfig{}, &mem, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(second).ok());
    bh::DailySummary reloaded[4] = {};
    TEST_ASSERT_EQUAL_UINT32(2, second.summaries(reloaded, 4));
    TEST_ASSERT_EQUAL_MEMORY(&ring[0], &reloaded[0], sizeof(bh::DailySummary));
    TEST_ASSERT_EQUAL_MEMORY(&ring[1], &reloaded[1], sizeof(bh::DailySummary));
}

void test_seq_exhaustion_cursor_never_wraps(void) {
    // Audit finding 9: a persisted cursor at kSeqMax would make
    // cursor_ + 1 wrap to 0 in step() and replay the WHOLE log (double
    // counting). step() applies the same seq-exhaustion early-out as the
    // probe/rebuild readers (§10).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    (void)seedTwoDays(store);

    // Hand-built meta: schema 1, cursor = kSeqMax (LE), regressions 0, count 0.
    uint8_t meta[bh::kMetaBlobBytes] = {};
    meta[0] = bh::kMetaSchemaVersion;
    meta[1] = 0xFF;
    meta[2] = 0xFF;
    meta[3] = 0xFF;
    meta[4] = 0xFF;
    meta[9] = 0;  // no slots
    TEST_ASSERT_TRUE(kv.put(bh::kKvKeyMeta, meta, sizeof(meta)).ok());

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    TEST_ASSERT_EQUAL_UINT32(bh::kSeqMax, agg.lastProcessedSeq());

    TEST_ASSERT_TRUE(agg.step().ok());
    TEST_ASSERT_TRUE(agg.step().ok());  // and stays out (no wrap to 0)
    TEST_ASSERT_EQUAL_UINT32(bh::kSeqMax, agg.lastProcessedSeq());
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());  // nothing replayed
}
