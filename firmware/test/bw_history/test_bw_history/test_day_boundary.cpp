// F16 §9 T2: day-keying and boundary rules (spec §6 D5/D6) — start-time
// attribution across the 04:00 boundary, exact-boundary instants, the
// [09:00, 21:00) daytime window edges and a multi-day episode.

#include <cstring>

#include <unity.h>

#include "history_test_support.h"

namespace {

using namespace f16_test;
namespace bh = bw::history;

// Runs one fixture through a fresh aggregator and returns the finalized
// record for `day_key` (the fixture appends one extra event on day
// `day_key + 1` so the day closes).
bh::DailySummary summarizeOneDay(const bw::Event& in_day, uint32_t day_key) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<> store;
    bw::testkit::InMemoryKeyValueStore<> kv;
    (void)appendEvent(store, bootEvent(1, 100));
    (void)appendEvent(store, in_day);
    const uint32_t seq_end = appendEvent(store, napCompleted(1, wallUnix(day_key + 1, 8, 0), 9000));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    (void)f16_test::bootToReady(agg);
    drainTo(agg, seq_end);

    bh::DailySummary out[4] = {};
    const size_t n = agg.summaries(out, 4);
    for (size_t i = 0; i < n; ++i) {
        if (out[i].day_key == day_key) {
            return out[i];
        }
    }
    bh::DailySummary none{};
    none.day_key = UINT32_MAX;  // sentinel: record not found
    return none;
}

}  // namespace

void test_day_boundary_night_episode_belongs_to_start_day(void) {
    // 23:50 -> 07:10 (7 h 20 min) starts in the logical day that ENDS at
    // 04:00; the full duration lands there (D5 start-time attribution).
    const bh::DailySummary s = summarizeOneDay(
        sleepClose(wallUnix(10, 23, 50), 2000, 26'400'000, bw::EpisodeClass::MainSleep), 10);
    TEST_ASSERT_EQUAL_UINT32(10, s.day_key);
    TEST_ASSERT_EQUAL_UINT32(26'400'000, s.total_sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s.daytime_sleep_ms);  // 23:50 is outside [9,21)
}

void test_day_boundary_exact_boundary_instants(void) {
    // The two instants are 878399 (wall 03:59:59 of calendar day 10) and
    // 878400 (wall 04:00:00 of the SAME calendar day) — one second apart,
    // straddling the 04:00 boundary. With the corrected D5 key (the calendar
    // day the logical day ENDS on): 03:59:59 still belongs to logical day 10
    // (whose [04:00, 04:00) window closes there); 04:00:00 opens logical day
    // 11. The boundary is exact — no instant belongs to both days.
    bh::DailySummary at_end = summarizeOneDay(
        sleepClose(wallUnix(10, 3, 59, 59), 2000, 3'600'000, bw::EpisodeClass::Rest), 10);
    TEST_ASSERT_EQUAL_UINT32(10, at_end.day_key);
    TEST_ASSERT_EQUAL_UINT32(3'600'000, at_end.total_sleep_ms);

    bh::DailySummary at_start = summarizeOneDay(
        sleepClose(wallUnix(11, 4, 0, 0), 2000, 3'600'000, bw::EpisodeClass::Rest), 11);
    TEST_ASSERT_EQUAL_UINT32(11, at_start.day_key);
    TEST_ASSERT_EQUAL_UINT32(3'600'000, at_start.total_sleep_ms);
}

void test_day_boundary_daytime_window_edges(void) {
    // D6: [09:00, 21:00) start-time window — 09:00 and 20:59:59 count,
    // 08:59:59 and 21:00:00 do not.
    const bh::DailySummary at_open = summarizeOneDay(
        sleepClose(wallUnix(10, 9, 0, 0), 2000, 600'000, bw::EpisodeClass::Rest), 10);
    TEST_ASSERT_EQUAL_UINT32(600'000, at_open.daytime_sleep_ms);

    const bh::DailySummary last_minute = summarizeOneDay(
        sleepClose(wallUnix(10, 20, 59, 59), 2000, 600'000, bw::EpisodeClass::Rest), 10);
    TEST_ASSERT_EQUAL_UINT32(600'000, last_minute.daytime_sleep_ms);

    const bh::DailySummary before_open = summarizeOneDay(
        sleepClose(wallUnix(10, 8, 59, 59), 2000, 600'000, bw::EpisodeClass::Rest), 10);
    TEST_ASSERT_EQUAL_UINT32(0, before_open.daytime_sleep_ms);

    const bh::DailySummary at_close = summarizeOneDay(
        sleepClose(wallUnix(10, 21, 0, 0), 2000, 600'000, bw::EpisodeClass::Rest), 10);
    TEST_ASSERT_EQUAL_UINT32(0, at_close.daytime_sleep_ms);
}

void test_day_boundary_multi_day_episode_keeps_full_duration(void) {
    // A 3-day episode contributes its FULL duration to its start day (D5).
    const bh::DailySummary s = summarizeOneDay(
        sleepClose(wallUnix(10, 12, 0), 2000, 3u * 86'400'000u, bw::EpisodeClass::MainSleep), 10);
    TEST_ASSERT_EQUAL_UINT32(10, s.day_key);
    TEST_ASSERT_EQUAL_UINT32(3u * 86'400'000u, s.total_sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(3u * 86'400'000u, s.main_sleep_ms);
    // 12:00 is daytime, and a MainSleep starting at 10:00 after a night
    // shift IS daytime sleep (D6, any class counts).
    TEST_ASSERT_EQUAL_UINT32(3u * 86'400'000u, s.daytime_sleep_ms);
}

void test_day_boundary_midnight_wall_is_same_logical_day(void) {
    // Wall 00:30 on calendar day 11 still belongs to logical day 10 (the day
    // that ends at 04:00) — the boundary is logical, not wall-midnight.
    const bh::DailySummary s = summarizeOneDay(
        sleepClose(wallUnix(10, 0, 30), 2000, 1'800'000, bw::EpisodeClass::Nap), 10);
    TEST_ASSERT_EQUAL_UINT32(10, s.day_key);
    TEST_ASSERT_EQUAL_UINT32(1'800'000, s.total_sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s.daytime_sleep_ms);  // 00:30 outside [9,21)
}
