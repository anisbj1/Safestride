// spec §9(b) + §15.2 D7: the per-type de-dup (EventRateLimiter). A suppressed
// repeat is de-duplication, not loss: publish still returns Ok, the counter
// ticks, and the FIRST occurrence is always kept (§15.3).

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/eventlog/event_rate_limiter.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;

void test_rate_sensor_fault_suppressed_inside_window(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1000)).ok());
    // 500 ms later: inside kMinRepeatIntervalSensorFaultMs (1000) → suppressed.
    const bw::Result<void> second = logger.publish(makeEvent(bw::EventType::SensorFault, 1500));
    TEST_ASSERT_TRUE(second.ok());  // D7: de-dup is success, not an error
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().rate_limited);

    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1500));
    TEST_ASSERT_EQUAL_UINT32(1, store.count());  // only the first reached the log
    TEST_ASSERT_EQUAL_UINT32(1000, store.at(0).t_start_ms);
}

void test_rate_sensor_fault_allowed_at_exact_interval(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1000)).ok());
    // Boundary: exactly 1000 ms later is NOT "< interval" → allowed (§15.3).
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 2000)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().rate_limited);
}

void test_rate_boundary_one_ms_inside_is_suppressed(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1000)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1999)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());  // 999 ms < 1000 → suppressed
    // A suppressed repeat does not move the window: the ORIGINAL time stands.
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 2000)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());
}

void test_rate_device_not_worn_5000_window(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DeviceNotWorn, 1000)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DeviceNotWorn, 5999)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());  // 4999 < 5000 → suppressed
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DeviceNotWorn, 6000)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());  // exactly 5000 → allowed
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().rate_limited);
}

void test_rate_battery_warning_60000_window(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::BatteryWarning, 1000)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::BatteryWarning, 60999)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());  // 59999 < 60000 → suppressed
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::BatteryWarning, 61000)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());
}

void test_rate_unlimited_type_never_suppressed(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    for (int i = 0; i < 5; ++i) {  // transition events: interval 0 (§15.3)
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 1000)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(5, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().rate_limited);
}

void test_rate_per_type_independence(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1000)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1100)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 1100)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());  // the SleepEpisode is unaffected
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().rate_limited);
}

void test_rate_no_time_reference_passes_through(void) {
    // No clock AND no producer time → nothing honest to compare (§15.7: the
    // limiter must never block): three same-instant faults all pass.
    EventLogger logger{nullptr, nullptr};
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 0)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(3, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().rate_limited);
}

void test_rate_storm_cannot_fill_queue(void) {
    // Property: de-dup caps a single-type storm far below the queue depth —
    // 100 same-type faults inside one second collapse to one queued record.
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    clock.setMs(10000);
    for (int i = 0; i < 100; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 10000 + i)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(99, logger.stats().rate_limited);
}
