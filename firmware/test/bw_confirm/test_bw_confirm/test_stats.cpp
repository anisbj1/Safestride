// F14 label stats (plan §9.6 / DoD §14): confirmed / rejected / unlabeled
// counters across mixed timelines, plus the drop and failure channels.

#include <unity.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

void test_stats_zero_initially(void) {
    ConfirmFixture f;
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.confirmed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rejected);
    TEST_ASSERT_EQUAL_UINT32(0, stats.unlabeled);
    TEST_ASSERT_EQUAL_UINT32(0, stats.dropped_unlabeled);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_giveups);
    TEST_ASSERT_EQUAL_UINT32(0, stats.publish_failures);
}

// One script exercising every counter path (spec §9: timeline + overflow):
//   drop 100 (overflow) → confirm 101 → timeout 102 → reject 103 → confirm 104
void test_stats_mixed_timeline_counts(void) {
    ConfirmFixture f;
    for (uint32_t seq = 100; seq < 105; ++seq) {
        f.mgr.onEvent(makeEvent(seq, bw::EventType::DrowsinessEvent, 1000 + seq));
    }
    f.mgr.step(2000);
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().dropped_unlabeled);  // 100

    f.mgr.onAlertStatus(makeAlert(true, 3000));
    f.mgr.step(8000);  // offer 101, deadline 38000
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(101, 8500)));
    f.mgr.step(8600);  // offer 102, deadline 38600

    f.mgr.step(38600);  // 102 times out → 103 offered, deadline 68600
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().unlabeled);
    TEST_ASSERT_EQUAL_UINT32(103, f.state().seq);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(103, 39000)));
    f.mgr.step(39100);  // offer 104, deadline 69100
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(104, 39500)));
    f.mgr.step(39600);  // queue empty → Idle

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(2, stats.confirmed);          // 101, 104
    TEST_ASSERT_EQUAL_UINT32(1, stats.rejected);           // 103
    TEST_ASSERT_EQUAL_UINT32(1, stats.unlabeled);          // 102
    TEST_ASSERT_EQUAL_UINT32(1, stats.dropped_unlabeled);  // 100
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_giveups);
    TEST_ASSERT_EQUAL_UINT32(0, stats.publish_failures);

    TEST_ASSERT_EQUAL_UINT(3, f.store.callCount());  // 101, 103, 104
    TEST_ASSERT_NULL(f.store.find(102));
    TEST_ASSERT_NULL(f.store.find(100));
    TEST_ASSERT_EQUAL_UINT(4, f.sink.count());  // 1 drop marker + 3 labels
}

void test_stats_publish_failure_counted(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);
    f.sink.failNext(1);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    // The publish Result is handled (counted), and the store path is not
    // affected by a failing sink.
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().publish_failures);
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().confirmed);
    TEST_ASSERT_EQUAL_UINT(1, f.store.callCount());
}

// DoD §11 budget: all F14 static state is the manager instance (queue,
// offered entry, retry buffer, stats, provider state) — measured as
// sizeof(), the host-side estimate the spec §11 table derives from.
void test_stats_manager_state_within_ram_budget(void) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(512, sizeof(bw::confirm::ConfirmationManager));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(80, sizeof(bw::confirm::PendingEventQueue));
}
