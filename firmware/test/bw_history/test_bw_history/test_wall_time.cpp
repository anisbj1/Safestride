// F16 §9 T3: wall-time attribution (spec §6 D4, §10) — session summaries
// for unknown unix, IClock derivation inside the current boot, other boots
// never derived, separate sessions per boot.

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

}  // namespace

void test_unknown_wall_time_session_summary_per_boot(void) {
    // All events have unix == 0 and the clock is unsynced: aggregate per
    // boot_id into a flagged session summary with day_key == 0 (§10) —
    // never a guessed date.
    bw::testkit::FakeClock clock;  // starts at unix 0 = unknown (DEC-009)
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(5, 100));
    (void)appendEvent(store, napCompleted(5, 0, 1000));
    (void)appendEvent(store, sleepClose(0, 2000, 3'600'000, bw::EpisodeClass::Uncertain, 5));
    const uint32_t seq_end = appendEvent(store, ignoredDrowsy(0, 3000, 5));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    // No later day ever arrives, so the session stays open and the ring is
    // empty — the record exists as soon as a different identity finalizes it.
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());
}

void test_unknown_wall_time_session_finalizes_on_next_boot(void) {
    // A second boot's events give the first session a different identity
    // (day_key 0, boot_id differs) and finalize it (§6 D4(a)).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(5, 100));
    const uint32_t seq_nap = appendEvent(store, napCompleted(5, 0, 1000));
    (void)appendEvent(store, sleepClose(0, 2000, 3'600'000, bw::EpisodeClass::Uncertain, 5));
    (void)appendEvent(store, bootEvent(6, 5000));
    const uint32_t seq_nap2 = appendEvent(store, napCompleted(6, 0, 6000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_nap2);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].day_key);  // session, not a day
    TEST_ASSERT_EQUAL_UINT32(5, ring[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32(seq_nap, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].flags);  // no derivation happened
    TEST_ASSERT_EQUAL_UINT8(1, ring[0].nap_count);
    TEST_ASSERT_EQUAL_UINT8(1, ring[0].episode_count);
    TEST_ASSERT_EQUAL_UINT32(3'600'000, ring[0].total_sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].daytime_sleep_ms);  // no wall time
}

void test_clock_derivation_within_current_boot(void) {
    // D4(c): unix == 0 but same boot_id + synced clock ⇒ derive
    // derived_unix = clock_unix - (now_ms - t_start_ms)/1000, flagged
    // kFlagWallTimeDerived; the episode lands in the derived day.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    const bw::UnixSeconds now_unix = wallUnix(7, 12, 0);  // wall noon, day 7
    clock.setUnixSeconds(now_unix);
    clock.setMs(10'000'000);

    (void)appendEvent(store, bootEvent(7, 100));
    // 1 h before "now": derived wall 11:00, same logical day 7, daytime.
    const uint32_t seq_sleep = appendEvent(
        store, sleepClose(0, 10'000'000 - 3'600'000, 1'800'000, bw::EpisodeClass::Nap, 7));
    const uint32_t seq_end = appendEvent(store, napCompleted(7, wallUnix(8, 8, 0), 11'000'000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    TEST_ASSERT_EQUAL_UINT32(7, agg.currentBootId());
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(7, ring[0].day_key);  // derived, not a session
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].boot_id);  // calendar day record
    TEST_ASSERT_EQUAL_UINT32(seq_sleep, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT8(bh::kFlagWallTimeDerived, ring[0].flags & bh::kFlagWallTimeDerived);
    TEST_ASSERT_EQUAL_UINT32(1'800'000, ring[0].total_sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(1'800'000, ring[0].daytime_sleep_ms);  // 11:00 in [9,21)
}

void test_other_boot_never_derived(void) {
    // D4: an event from ANOTHER boot with unix == 0 must not be mapped
    // through the current clock — that would fabricate a day (§10). It
    // becomes a session summary even though the clock is synced.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    clock.setUnixSeconds(wallUnix(7, 12, 0));
    clock.setMs(10'000'000);

    (void)appendEvent(store, bootEvent(7, 100));
    // boot_id 9 events were appended in a PREVIOUS boot (t_start_ms on that
    // boot's timeline — here indistinguishable by value, hence the rule).
    const uint32_t seq_old =
        appendEvent(store, sleepClose(0, 5'000'000, 3'600'000, bw::EpisodeClass::MainSleep, 9));
    const uint32_t seq_end = appendEvent(store, napCompleted(7, wallUnix(8, 8, 0), 11'000'000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].day_key);  // session, NOT a derived day
    TEST_ASSERT_EQUAL_UINT32(9, ring[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32(seq_old, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT8(0, ring[0].flags);
    TEST_ASSERT_EQUAL_UINT32(3'600'000, ring[0].total_sleep_ms);
}

void test_unknown_then_synced_switches_identity(void) {
    // Same boot: unknown wall time first (session), clock sync arrives, the
    // next unknown-unix event derives into a day — identity switch
    // finalizes the session (§6 D4(c) then (a) fallback).
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    (void)appendEvent(store, bootEvent(10, 100));
    const uint32_t seq_sess = appendEvent(store, napCompleted(10, 0, 1000));  // clock unknown

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_sess);

    // Clock sync arrives mid-boot; the next event derives.
    const bw::UnixSeconds now_unix = wallUnix(9, 15, 0);
    clock.setUnixSeconds(now_unix);
    clock.setMs(1'000'000);
    const uint32_t seq_derived =
        appendEvent(store, sleepClose(0, 1'000'000 - 600'000, 900'000, bw::EpisodeClass::Nap, 10));
    const uint32_t seq_end = appendEvent(store, napCompleted(10, wallUnix(10, 8, 0), 2'000'000));
    drainTo(agg, seq_end);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(2, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].day_key);  // session first (older)
    TEST_ASSERT_EQUAL_UINT32(10, ring[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32(seq_sess, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT32(9, ring[1].day_key);  // 15:00 - 10 min = 14:50, day 9
    TEST_ASSERT_EQUAL_UINT32(seq_derived, ring[1].first_seq);
    TEST_ASSERT_EQUAL_UINT8(bh::kFlagWallTimeDerived, ring[1].flags & bh::kFlagWallTimeDerived);
}

void test_stale_boot_event_never_fabricates_day(void) {
    // §6 D4 + audit A2: the CURRENT boot's Boot event is missing — the log
    // holds only Boot(1) plus unix-0 events of boot 1 AND boot 2 (boot_id 2
    // proves a newer boot exists whose Boot event never landed). The D4
    // boot_id check alone then matches boot-1 events against the stale
    // current_boot_id_ and maps their monotonic t_start_ms through the
    // CURRENT clock — a fabricated day. With the stale anchor every event
    // must stay a session summary: no derived day, ever.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    clock.setUnixSeconds(wallUnix(7, 12, 0));
    clock.setMs(10'000'000);

    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, napCompleted(1, 0, 1000));  // boot-1 event, unix 0
    const uint32_t seq_end =
        appendEvent(store, sleepClose(0, 2000, 3'600'000, bw::EpisodeClass::MainSleep, 2));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    // The boot-2 event finalizes whatever the boot-1 event opened: it must
    // be a session, not a fabricated calendar day (pre-fix: day 7 appeared).
    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].day_key);  // session, NOT a derived day
    TEST_ASSERT_EQUAL_UINT32(1, ring[0].boot_id);
    TEST_ASSERT_EQUAL_UINT8(0, ring[0].flags);            // no derivation happened
    TEST_ASSERT_EQUAL_UINT32(0, agg.clockRegressions());  // sessions never hit D9
}

void test_missing_boot_event_never_fabricates_day(void) {
    // Audit A2 (regression guard): NO Boot event anywhere — the anchor is
    // unknown, so unix-0 events with a perfectly synced clock must still
    // aggregate as a session summary, never clock-derived into a day.
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    clock.setUnixSeconds(wallUnix(7, 12, 0));
    clock.setMs(10'000'000);

    const uint32_t seq_nap = appendEvent(store, napCompleted(5, 0, 1000));
    (void)appendEvent(store, sleepClose(0, 2000, 3'600'000, bw::EpisodeClass::MainSleep, 5));
    const uint32_t seq_end = appendEvent(store, napCompleted(5, wallUnix(3, 10, 0), 3000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    TEST_ASSERT_TRUE(f16_test::bootToReady(agg).ok());
    drainTo(agg, seq_end);

    bh::DailySummary ring[4] = {};
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaries(ring, 4));
    TEST_ASSERT_EQUAL_UINT32(0, ring[0].day_key);  // session, never a day
    TEST_ASSERT_EQUAL_UINT32(5, ring[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32(seq_nap, ring[0].first_seq);
    TEST_ASSERT_EQUAL_UINT8(0, ring[0].flags);  // no derivation
    TEST_ASSERT_EQUAL_UINT32(3'600'000, ring[0].total_sleep_ms);
}
