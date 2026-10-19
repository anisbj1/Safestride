// spec §9(c) + §15.2 D10: wall time and the F17-generated records. Unknown
// wall time stays an honest 0 all the way into the store (DEC-009); the
// logger emits Event{Boot} once and Event{TimeSync} on genuine re-syncs only —
// never on ordinary ticking, and it tolerates a wall clock that never ticks
// (F00 §13: the device IClock may only change on phone SetTime).

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;

void test_boot_emitted_once_with_detector_17(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    logger.emitBoot();
    logger.emitBoot();  // idempotent (§15.2 D10): exactly one per boot
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(0));

    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    const bw::Event& boot = store.at(0);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(boot.type));
    TEST_ASSERT_EQUAL_UINT32(17, boot.detector_id);
    TEST_ASSERT_EQUAL_UINT32(1, boot.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, boot.confidence);  // factual record
    // F17's own events carry no sensor stream: honest Insufficient (00 §1),
    // not a fabricated Good quality.
    TEST_ASSERT_EQUAL(static_cast<int>(bw::DataQuality::Insufficient),
                      static_cast<int>(boot.quality.imu));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::DataQuality::Insufficient),
                      static_cast<int>(boot.quality.ppg));
}

void test_boot_t_start_ms_filled_from_clock(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(4242);
    logger.emitBoot();
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(4242));
    TEST_ASSERT_EQUAL_UINT32(4242, store.at(0).t_start_ms);
}

void test_wall_unknown_stays_zero_into_store(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);  // unixSeconds() stays 0: no phone sync yet (DEC-009)
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1000)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));
    TEST_ASSERT_EQUAL_UINT32(0, store.at(0).t_start_unix);
}

void test_wall_known_stamps_at_publish(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(5000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1000)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(1000));  // + the first TimeSync (wall known)
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL_UINT32(5000, store.at(0).t_start_unix);
}

void test_wall_producer_captured_value_preserved(void) {
    // F08 captures t_start_unix at candidate start; F17 must never overwrite a
    // producer's value, only fill an empty one (§15.2 D10).
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(5000);
    bw::Event producer = makeEvent(bw::EventType::SleepEpisode, 1000);
    producer.t_start_unix = 777;
    TEST_ASSERT_TRUE(logger.publish(producer).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(1000));  // event + first TimeSync
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL_UINT32(777, store.at(0).t_start_unix);
}

void test_timesync_emitted_once_when_wall_becomes_known(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(500);
    logger.emitBoot();
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(500));  // Boot only; wall still unknown
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL_UINT32(0, store.at(0).t_start_unix);

    clock.setMs(1000);
    clock.setUnixSeconds(1000);  // phone SetTime → unknown→known
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));
    TEST_ASSERT_EQUAL_UINT32(2, store.count());
    const bw::Event& sync = store.at(1);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::TimeSync), static_cast<int>(sync.type));
    TEST_ASSERT_EQUAL_UINT32(1000, sync.t_start_unix);  // the newly-known value travels
    TEST_ASSERT_EQUAL_UINT32(17, sync.detector_id);
    TEST_ASSERT_EQUAL_UINT32(1000, sync.t_start_ms);
}

void test_timesync_static_wall_clock_never_repeats(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(1000);
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));  // the one TimeSync
    for (int i = 2; i <= 10; ++i) {                   // wall value never moves
        clock.setMs(i * 1000);
        TEST_ASSERT_EQUAL_UINT32(0, logger.drain(i * 1000));
    }
    TEST_ASSERT_EQUAL_UINT32(1, store.count());
}

void test_timesync_ticking_clock_never_emits(void) {
    // A clock whose wall value ticks with monotonic time is the normal case:
    // 50 one-second ticks must produce ZERO extra TimeSync records.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(1000);
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));  // first known → one record
    for (int i = 1; i <= 50; ++i) {
        clock.setMs(1000 + i * 100);
        if (i % 10 == 0) {  // each whole second, the wall value advances by 1
            clock.setUnixSeconds(1000 + i / 10);
        }
        TEST_ASSERT_EQUAL_UINT32(0, logger.drain(1000 + i * 100));
    }
    TEST_ASSERT_EQUAL_UINT32(1, store.count());
}

void test_timesync_jump_beyond_tolerance_emits(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(1000);
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));
    clock.setMs(2000);
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(2000));  // wall unchanged: not a sync
    clock.setMs(62000);
    clock.setUnixSeconds(4600);  // +1 h far beyond kWallResyncToleranceS: a SetTime
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(62000));
    TEST_ASSERT_EQUAL_UINT32(2, store.count());
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::TimeSync),
                      static_cast<int>(store.at(1).type));
    TEST_ASSERT_EQUAL_UINT32(4600, store.at(1).t_start_unix);
}

void test_timesync_backwards_set_emits(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(1000);
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));
    clock.setMs(2000);
    clock.setUnixSeconds(500);  // phone corrects backwards — still a real sync
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(2000));
    TEST_ASSERT_EQUAL_UINT32(2, store.count());
    TEST_ASSERT_EQUAL_UINT32(500, store.at(1).t_start_unix);
}

void test_wall_time_without_clock_never_crashes(void) {
    // Empty composition context (§15.7): no clock, no fault sink — honest
    // zeros everywhere, every path reachable without a dereference.
    FakeEventStore<> store;
    EventLogger logger{nullptr, nullptr};
    logger.attachStore(&store);
    logger.emitBoot();
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 0)).ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(0));  // unix 0 → no TimeSync attempt
    TEST_ASSERT_EQUAL_UINT32(0, store.at(0).t_start_unix);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().store_fault_episodes);
}

void test_wall_unknown_at_publish_stays_zero_when_clock_syncs_before_drain(void) {
    // spec §15.6 item 6 — the isolating case: t_start_unix is stamped at PUBLISH
    // time, so an event queued while the wall clock was still unknown keeps 0
    // even when the phone syncs before the drain runs. If stamping ever moved
    // to drain time, at(0).t_start_unix would read 5000 and this would fail.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);  // wall clock still unknown (unix 0)
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1000)).ok());
    clock.setUnixSeconds(5000);                       // phone syncs BEFORE the drain
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(1000));  // event + the first TimeSync

    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL_UINT32(0, store.at(0).t_start_unix);  // honest zero, not 5000
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::TimeSync),
                      static_cast<int>(store.at(1).type));
    TEST_ASSERT_EQUAL_UINT32(5000, store.at(1).t_start_unix);  // the sync is its own record
}
