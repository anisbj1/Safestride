// bw_reliability — ReinitScheduler tests (spec §16.8 item 5): the D12
// exponential backoff ladder (1 s -> 2 s -> 4 s ... capped at 30 s) and the
// recovery reset. Lands unwired (G1 ruling 3) — these tests pin the pacing
// contract for whoever adds the F01 execution seam later.

#include <bw/hardware.h>
#include <bw/reliability/reinit_scheduler.h>
#include <bw/reliability/reliability_config.h>

#include <unity.h>

namespace {

using bw::SensorStatus;
using bw::reliability::ReinitScheduler;

}  // namespace

// The first not-Ok observation is DUE immediately (nothing armed yet).
void test_reinit_first_attempt_is_due_immediately(void) {
    ReinitScheduler s;
    TEST_ASSERT_TRUE(s.attemptDue(0, SensorStatus::Failed));
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kReinitBackoffInitialMs, s.nextAttemptMs());
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kReinitBackoffInitialMs * 2, s.backoffMs());
}

// The ladder doubles per accepted attempt and caps at
// kReinitBackoffMaxMs: 1 s, 2 s, 4 s, 8 s, 16 s, 30 s, 30 s ... (spec §8/D12)
// — a dead bus is never hammered (row 10). `expected` = the gap each accepted
// attempt schedules before the next one.
void test_reinit_backoff_doubles_to_cap(void) {
    ReinitScheduler s;
    bw::TimeMs now = 0;

    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (const uint32_t want : expected) {
        TEST_ASSERT_TRUE_MESSAGE(s.attemptDue(now, SensorStatus::Failed), "attempt should be due");
        TEST_ASSERT_EQUAL_UINT64(now + want, s.nextAttemptMs());
        now = s.nextAttemptMs();  // exactly at the horizon -> due again
    }
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kReinitBackoffMaxMs, s.backoffMs());
}

// Inside the horizon the answer is false — that is the anti-hammer rule.
void test_reinit_not_due_before_horizon(void) {
    ReinitScheduler s;
    TEST_ASSERT_TRUE(s.attemptDue(1000, SensorStatus::Uninitialized));
    const bw::TimeMs next = s.nextAttemptMs();
    TEST_ASSERT_FALSE(s.attemptDue(next - 1, SensorStatus::Uninitialized));
    TEST_ASSERT_TRUE(s.attemptDue(next, SensorStatus::Uninitialized));
}

// Recovery: Ok cancels the pending attempt and restarts the ladder at
// kReinitBackoffInitialMs — a healthy sensor begins fresh next time.
void test_reinit_ok_cancels_and_resets_backoff(void) {
    ReinitScheduler s;
    (void)s.attemptDue(0, SensorStatus::Failed);     // ladder -> 2000
    (void)s.attemptDue(1000, SensorStatus::Failed);  // ladder -> 4000
    TEST_ASSERT_EQUAL_UINT32(4000, s.backoffMs());

    TEST_ASSERT_FALSE(s.attemptDue(1500, SensorStatus::Ok));  // recovery: never due
    TEST_ASSERT_EQUAL_UINT32(0, s.nextAttemptMs());           // pending cancelled
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kReinitBackoffInitialMs, s.backoffMs());

    // Next failure starts over at the initial backoff.
    TEST_ASSERT_TRUE(s.attemptDue(2000, SensorStatus::Failed));
    TEST_ASSERT_EQUAL_UINT32(bw::reliability::kReinitBackoffInitialMs * 2, s.backoffMs());
}
