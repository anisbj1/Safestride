#include <unity.h>

#include "history_test_support.h"

namespace {
namespace bh = bw::history;
}

void test_boot_restore_is_bounded_and_hides_summaries_until_ready(void) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<64> memory;
    bw::testkit::InMemoryKeyValueStore<> kv;
    for (uint32_t seq = 0; seq < 24; ++seq) {
        (void)f16_test::appendEvent(memory,
                                    f16_test::ignoredDrowsy(f16_test::wallUnix(70, 10), seq));
    }
    f16_test::CountingEventStore<64> store(memory);
    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};

    const bw::Result<void> start = agg.boot();
    TEST_ASSERT_FALSE(start.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::Busy),
                             static_cast<uint32_t>(start.err));
    TEST_ASSERT_EQUAL_UINT32(0, store.read_calls);  // wiring never scans the event log
    TEST_ASSERT_FALSE(agg.ready());
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());
    bh::DailySummary latest{};
    TEST_ASSERT_FALSE(agg.latestDailySummary(latest));

    // Each service-pump call performs no more than one bounded log read.
    // This fixture needs multiple probe batches, meta restore, then ready.
    size_t prior_reads = store.read_calls;
    constexpr size_t kMaxBootTicks = 64;  // [DERIVED] 24 events / 8 + phase transitions
    for (size_t tick = 0; tick < kMaxBootTicks && !agg.ready(); ++tick) {
        const bw::Result<void> result = agg.step();
        TEST_ASSERT_TRUE(result.ok() || result.err == bw::Err::Busy);
        TEST_ASSERT_TRUE(store.read_calls - prior_reads <= 1);
        TEST_ASSERT_TRUE(store.largest_read_limit <= bh::kAggMaxEventsPerTick);
        prior_reads = store.read_calls;
    }
    TEST_ASSERT_TRUE(agg.ready());
    TEST_ASSERT_EQUAL_UINT32(0, agg.summaryCount());  // ignored events do not fabricate rows
}
