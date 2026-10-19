// SensorStatusTracker tests (F01 §10 failure behaviour: N consecutive failures
// -> Failed + one fault report; absent at boot -> Err::NotFound path; replug).

#include <unity.h>

#include <bw/hal/sensor_status.h>

using bw::SensorStatus;
using bw::hal::SensorStatusTracker;

namespace {
constexpr uint8_t kFailures = 5;  // F01 §8 kFailuresBeforeFailed
}

void test_sensor_status_fresh_tracker_is_uninitialized(void) {
    SensorStatusTracker tracker(kFailures);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Uninitialized),
                          static_cast<int>(tracker.status()));
}

void test_sensor_status_begin_ok_marks_ok(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginOk();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(tracker.status()));
}

void test_sensor_status_absent_at_boot_is_failed_immediately(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginAbsent();  // begin() -> Err::NotFound (spec §10): no waiting
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed),
                          static_cast<int>(tracker.status()));
    TEST_ASSERT_EQUAL(false, tracker.readFail());  // already Failed: no new edge
}

void test_sensor_status_failures_below_threshold_stay_ok(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginOk();
    for (uint8_t i = 0; i < kFailures - 1; ++i) {
        TEST_ASSERT_EQUAL(false, tracker.readFail());
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(tracker.status()));
}

void test_sensor_status_nth_consecutive_failure_transitions_once(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginOk();
    for (uint8_t i = 0; i < kFailures - 1; ++i) {
        TEST_ASSERT_EQUAL(false, tracker.readFail());
    }
    TEST_ASSERT_EQUAL(true, tracker.readFail());  // the edge: driver reports once
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed),
                          static_cast<int>(tracker.status()));
    TEST_ASSERT_EQUAL(false, tracker.readFail());  // still broken: no repeat edge
    TEST_ASSERT_EQUAL(false, tracker.readFail());
}

void test_sensor_status_success_resets_the_streak(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginOk();
    for (uint8_t i = 0; i < kFailures - 1; ++i) {
        tracker.readFail();
    }
    tracker.readOk();  // one good read re-arms: a flapping sensor never fails
    TEST_ASSERT_EQUAL(false, tracker.readFail());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(tracker.status()));
}

void test_sensor_status_reinit_after_failure_restores_ok(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginOk();
    for (uint8_t i = 0; i < kFailures - 1; ++i) {
        tracker.readFail();
    }
    TEST_ASSERT_EQUAL(true, tracker.readFail());
    tracker.beginOk();  // reinit() succeeded (when to call it: F26)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(tracker.status()));
    TEST_ASSERT_EQUAL(false, tracker.readFail());  // streak was cleared
}

void test_sensor_status_read_ok_after_absent_covers_replug(void) {
    SensorStatusTracker tracker(kFailures);
    tracker.beginAbsent();
    tracker.readOk();  // device reappeared on the bus without a reinit call
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(tracker.status()));
    for (uint8_t i = 0; i < kFailures - 1; ++i) {
        TEST_ASSERT_EQUAL(false, tracker.readFail());  // streak restarted fully
    }
}

void test_sensor_status_zero_failures_means_fail_on_first(void) {
    SensorStatusTracker tracker(0);  // defensive: 0 would be a silent "never"
    tracker.beginOk();
    TEST_ASSERT_EQUAL(true, tracker.readFail());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed),
                          static_cast<int>(tracker.status()));
}
