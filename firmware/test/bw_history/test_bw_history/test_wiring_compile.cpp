// F16 wiring sanity check (spec §9 T12): compiles wire_history.cpp into the
// host suite (wiring README §3 requires the pair to build for native too, so
// P4 finds no breakage), pins the §8 constants and checks the §10
// degradation path with null providers.

#include "../../../src/app/wiring/wire_history.cpp"

#include <unity.h>

namespace bh = bw::history;

void test_wiring_constants_match_spec(void) {
    TEST_ASSERT_EQUAL_UINT32(4, bh::kDayBoundaryHour);      // §8 [USER]
    TEST_ASSERT_EQUAL_UINT32(9, bh::kDaytimeStartHour);     // §8 [USER]
    TEST_ASSERT_EQUAL_UINT32(21, bh::kDaytimeEndHour);      // §8 [USER]
    TEST_ASSERT_EQUAL_UINT32(7, bh::kDeviceSummaryDays);    // §8 [EXPERIMENTAL]
    TEST_ASSERT_EQUAL_UINT32(8, bh::kAggMaxEventsPerTick);  // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(60000, bh::kAggTickPeriodMs);  // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(2, bh::kRebuildDays);          // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(33, bh::kSummaryBlobBytes);    // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(10, bh::kMetaBlobBytes);       // §8 [DERIVED]
}

void test_wiring_tick_before_init_is_notready(void) {
    // historyTick() before wireHistoryInit() answers honestly (§10) — this
    // test must stay FIRST among the wiring tests: the wiring below claims
    // the singleton.
    const bw::Result<void> r = f16::historyTick();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::NotReady),
                             static_cast<uint32_t>(r.err));
}

void test_wiring_null_providers_degrade_without_crash(void) {
    // Composition-root smoke test: an empty context and null providers must
    // not crash; boot()/tick() answer NotReady and no summary is fabricated
    // (§10, README §3 native-compile rule).
    app::AppContext ctx{};
    ctx.clock = nullptr;
    f16::wireHistoryInit(ctx, nullptr, nullptr);

    const bw::Result<void> r = f16::historyTick();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::Err::NotReady),
                             static_cast<uint32_t>(r.err));

    bh::DailySummary out{};
    TEST_ASSERT_FALSE(f16::historyLatestDailySummary(out));
}
