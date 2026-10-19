// bw_reliability — WatchdogSupervisor tests (spec §16.8 item 2): feed
// decision per D9 (Tier A green only), stall edges per rows 5/6, boot grace,
// pending-vs-stalled, and the backwards-clock guard.

#include <bw/reliability/reliability_config.h>
#include <bw/reliability/watchdog_supervisor.h>

#include <unity.h>

namespace {

using bw::reliability::BeatState;
using bw::reliability::ScanResult;
using bw::reliability::TaskConfig;
using bw::reliability::TaskId;
using bw::reliability::TaskTier;
using bw::reliability::WatchdogSupervisor;

// [DERIVED from kStallFactor] tier A period used by most cases: stall
// deadline = 3 x 1000 ms = 3000 ms.
constexpr bw::TimeMs kPeriodA = 1000;
// Tier B period: stall deadline = 3 x 100 ms = 300 ms.
constexpr bw::TimeMs kPeriodB = 100;

void configureSensorA(WatchdogSupervisor& sup, const bw::TimeMs period = kPeriodA) {
    sup.configure({TaskId::Sensor, TaskTier::A, static_cast<uint32_t>(period)});
}

}  // namespace

// D9 (a)+(c): Tier A configured, beat fresh after the grace window => feed.
void test_supervisor_all_fresh_feeds(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup);
    sup.beat(TaskId::Sensor, 11000);
    const ScanResult scan = sup.scan(11000);
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask);
    TEST_ASSERT_TRUE(scan.tier_a_all_fresh);
    TEST_ASSERT_TRUE(sup.shouldFeed(11000));
}

// Row 5 / D9 (b): a Tier A task past 3x its period stalls => the feed is
// WITHHELD (that is what turns a dead pipeline into a watchdog reset).
void test_supervisor_tier_a_stall_withholds_feed(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup);
    sup.beat(TaskId::Sensor, 10000);

    const ScanResult scan = sup.scan(13001);  // 3001 ms > 3 x 1000 ms
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.stalled_mask);
    TEST_ASSERT_EQUAL_UINT32(scan.stalled_mask, scan.newly_stalled_mask);
    TEST_ASSERT_FALSE(scan.tier_a_all_fresh);
    TEST_ASSERT_FALSE(sup.shouldFeed(13001));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BeatState::Stalled),
                          static_cast<int>(sup.state(TaskId::Sensor)));
}

// Row 6: a Tier B stall latches its bit (mapped to a facility by the policy)
// but never withholds the feed — BLE/display must not reset sensing (00 rule).
void test_supervisor_tier_b_stall_flags_but_keeps_feeding(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);
    sup.configure({TaskId::Ui, TaskTier::B, static_cast<uint32_t>(kPeriodB)});
    sup.beat(TaskId::Sensor, 10000);
    sup.beat(TaskId::Ui, 10000);

    const ScanResult scan = sup.scan(12000);  // Ui: 2000 > 300 stalled; Sensor: 2000 < 3000
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Ui), scan.stalled_mask);
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask & (1u << static_cast<uint8_t>(TaskId::Sensor)));
    TEST_ASSERT_TRUE(scan.tier_a_all_fresh);
    TEST_ASSERT_TRUE(sup.shouldFeed(12000));
}

// Grace window (kSupervisionGraceMs): inside it the feed flows even though
// not every Tier A task has beaten yet — staggered creation must not reset
// the board (§16.3 rationale).
void test_supervisor_grace_window_feeds_pending_tasks(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);  // configured, never beaten
    const ScanResult scan = sup.scan(5000);
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.pending_mask);
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask);  // pending != stalled (D7)
    TEST_ASSERT_TRUE(sup.shouldFeed(5000));          // inside 10 s grace
}

// After the grace, a configured-but-never-beaten Tier A task is NOT fresh:
// no history, no feed (fail-safe — it is not beating, we just cannot call it
// stalled yet).
void test_supervisor_pending_after_grace_does_not_feed(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);
    (void)sup.scan(11000);  // grace passed, still pending
    TEST_ASSERT_FALSE(sup.shouldFeed(11000));
}

// Fail-safe (D9 (a)): supervising nothing Tier A is a wiring fault — never
// feed, whatever Tier B is doing.
void test_supervisor_nothing_configured_never_feeds(void) {
    WatchdogSupervisor sup;
    sup.configure({TaskId::Ui, TaskTier::B, static_cast<uint32_t>(kPeriodB)});
    sup.beat(TaskId::Ui, 20000);
    (void)sup.scan(20000);
    TEST_ASSERT_FALSE(sup.shouldFeed(20000));
}

// Enter edge fires exactly once per stall episode (that is where TaskStalled
// is reported); a recovery beat clears the stall, a later death re-edges.
void test_supervisor_newly_stalled_fires_once_per_episode(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);
    sup.beat(TaskId::Sensor, 0);

    ScanResult scan = sup.scan(100);  // 100 ms beat: inside 3 x 1000 -> fresh
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask);

    scan = sup.scan(3001);  // enter edge
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.newly_stalled_mask);
    scan = sup.scan(6001);  // still stalled, but NOT newly again
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.stalled_mask);
    TEST_ASSERT_EQUAL_UINT32(0, scan.newly_stalled_mask);

    sup.beat(TaskId::Sensor, 6100);  // recovery
    scan = sup.scan(6200);
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask);
    TEST_ASSERT_TRUE(scan.tier_a_all_fresh);

    scan = sup.scan(10000);  // dead again: a NEW episode re-edges
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.newly_stalled_mask);
}

// Robustness: a backwards TimeMs jump must not underflow into "stalled since
// the dawn of time", and a huge forward gap is an ordinary stall.
void test_supervisor_clock_jumps_are_safe(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);
    sup.beat(TaskId::Sensor, 10000);

    ScanResult scan = sup.scan(5000);  // backwards jump
    TEST_ASSERT_EQUAL_UINT32(0, scan.stalled_mask);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BeatState::Fresh),
                          static_cast<int>(sup.state(TaskId::Sensor)));

    scan = sup.scan(1000000000ull);  // huge forward gap -> stall, not UB
    TEST_ASSERT_EQUAL_UINT32(1u << static_cast<uint8_t>(TaskId::Sensor), scan.stalled_mask);
}

// Re-configuration keeps the table consistent: a period-0 config is an
// argument error (ignored), and re-configuring a Tier A slot does not
// double-count it in the feed decision.
void test_supervisor_configure_is_argument_safe(void) {
    WatchdogSupervisor sup;
    configureSensorA(sup, 1000);
    sup.configure({TaskId::Sensor, TaskTier::A, 0});  // 0 period: ignored
    sup.beat(TaskId::Sensor, 11000);
    (void)sup.scan(11000);
    TEST_ASSERT_TRUE(sup.shouldFeed(11000));  // still one Tier A, healthy
}

void test_supervisor_rejects_sentinel_and_invalid_task_ids(void) {
    WatchdogSupervisor sup;
    sup.configure({TaskId::Count, TaskTier::A, static_cast<uint32_t>(kPeriodA)});
    TEST_ASSERT_EQUAL_UINT32(0, sup.scan(11000).pending_mask);
    const TaskId invalid_ids[] = {TaskId::Count,
                                  static_cast<TaskId>(bw::reliability::kMaxSupervisedTasks),
                                  static_cast<TaskId>(UINT8_MAX)};
    for (const TaskId id : invalid_ids) {
        sup.configure({id, TaskTier::A, static_cast<uint32_t>(kPeriodA)});
        sup.beat(id, 11000);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(BeatState::Pending),
                              static_cast<int>(sup.state(id)));
    }
    TEST_ASSERT_FALSE(sup.shouldFeed(11000));  // invalid configuration can't authorize a feed
    configureSensorA(sup);
    sup.beat(TaskId::Sensor, 11000);
    TEST_ASSERT_TRUE(sup.scan(11000).tier_a_all_fresh);  // valid slots remain usable
    TEST_ASSERT_TRUE(sup.shouldFeed(11000));
}
