// F07 §9 test_duration_counter.cpp — the dwell/recovery counters of D6/D8.

#include <bw/state/duration_counter.h>
#include <bw/state/state_config.h>
#include <unity.h>

using bw::state::DurationCounter;
using bw::state::kMaxDurationFieldMs;

void test_counter_accumulates_while_active(void) {
    DurationCounter c;
    c.update(true, 1000);
    c.update(true, 2000);
    c.update(true, 5000);  // 0 + 1 s + 3 s
    TEST_ASSERT_EQUAL_UINT32(4000, c.runMs(5000));
}

void test_counter_first_active_instant_is_zero(void) {
    // The run starts at the first active sample: the dwell measures time
    // HELD, not time since the call began.
    DurationCounter c;
    c.update(true, 12345);
    TEST_ASSERT_EQUAL_UINT32(0, c.runMs(12345));
    TEST_ASSERT_TRUE(c.sustained(12345, 0));  // running, and 0 >= 0 (inclusive)
}

void test_counter_resets_never_pauses(void) {
    // D8/E1: evidence must be continuous — a sample below enter RESETS the
    // dwell, it does not pause it.
    DurationCounter c;
    c.update(true, 0);
    c.update(true, 59000);
    c.update(false, 60000);  // dip: reset
    TEST_ASSERT_EQUAL_UINT32(0, c.runMs(60000));
    TEST_ASSERT_FALSE(c.running());
    c.update(true, 61000);  // evidence back: dwell restarts from zero
    c.update(true, 90000);
    TEST_ASSERT_EQUAL_UINT32(29000, c.runMs(90000));
}

void test_counter_sustained_inclusive_at_boundary(void) {
    DurationCounter c;
    c.update(true, 0);
    c.update(true, 59999);
    TEST_ASSERT_FALSE(c.sustained(59999, 60000));
    c.update(true, 60000);
    TEST_ASSERT_TRUE(c.sustained(60000, 60000));  // == counts (spec §9)
}

void test_counter_projection_reads_between_updates(void) {
    // sustained(now, needed) projects the run to `now` — the engine reads
    // once per tick after update(), and a projected read never underflows.
    DurationCounter c;
    c.update(true, 0);
    c.update(true, 10000);
    TEST_ASSERT_EQUAL_UINT32(15000, c.runMs(15000));
    TEST_ASSERT_TRUE(c.sustained(15000, 15000));
}

void test_counter_saturates_at_max_duration(void) {
    // 02 §4.5 fields are uint32: saturate instead of wrapping past 49 days.
    DurationCounter c;
    c.update(true, 0);
    c.update(true, kMaxDurationFieldMs);
    TEST_ASSERT_EQUAL_UINT32(kMaxDurationFieldMs, c.runMs(kMaxDurationFieldMs));
    c.update(true, static_cast<bw::TimeMs>(kMaxDurationFieldMs) + 100000);
    TEST_ASSERT_EQUAL_UINT32(kMaxDurationFieldMs, c.runMs(kMaxDurationFieldMs));
}

void test_counter_future_and_backward_now_never_underflow(void) {
    // 01 §4 says monotonic; a violated contract must not corrupt a dwell.
    DurationCounter c;
    c.update(true, 10000);
    c.update(true, 20000);
    // Reading with a stamp BEHIND the last update returns the stored run.
    TEST_ASSERT_EQUAL_UINT32(10000, c.runMs(15000));
    // An update with a stamp behind the last one restarts the run (time
    // discontinuity breaks evidence continuity, same as a dip).
    c.update(true, 12000);
    TEST_ASSERT_EQUAL_UINT32(0, c.runMs(12000));
    c.update(true, 22000);
    TEST_ASSERT_EQUAL_UINT32(10000, c.runMs(22000));
}

void test_counter_reset_clears_run(void) {
    DurationCounter c;
    c.update(true, 0);
    c.update(true, 5000);
    c.reset();
    TEST_ASSERT_FALSE(c.running());
    TEST_ASSERT_FALSE(c.sustained(5000, 0));
    TEST_ASSERT_EQUAL_UINT32(0, c.runMs(5000));
}
