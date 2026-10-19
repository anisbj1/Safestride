// spec §9 + §15.2 D10: record stamping at admission. F17 owns the record
// format — it stamps schema_version on EVERY event it accepts and fills only
// the time fields the producer left empty (never overwrites a producer value).
// Producer-owned fields (type, confidence, detector_id, reasons, payload)
// pass through byte-for-byte.

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;

void test_stamping_schema_version_always_current(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    // Producer leaves schema_version 0 (unset) — F17 stamps the current one.
    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(100));
    TEST_ASSERT_EQUAL_UINT32(bw::Event::kSchemaVersion, store.at(0).schema_version);
}

void test_stamping_overwrites_stale_schema_version(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    // A producer that stamps its own (wrong/old) version must be corrected —
    // F17 owns the record format (§15.2 D10), so the store always reads one
    // consistent schema.
    bw::Event producer = makeEvent(bw::EventType::SleepEpisode, 100);
    producer.schema_version = 99;
    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(producer).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(100));
    TEST_ASSERT_EQUAL_UINT32(bw::Event::kSchemaVersion, store.at(0).schema_version);
}

void test_stamping_t_start_ms_filled_only_when_zero(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(5000);

    // (a) producer left 0 -> filled from the clock.
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 0)).ok());
    // (b) producer set its own capture time -> preserved exactly.
    bw::Event captured = makeEvent(bw::EventType::SleepEpisode, 0);
    captured.t_start_ms = 4242;
    TEST_ASSERT_TRUE(logger.publish(captured).ok());

    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(5000));
    TEST_ASSERT_EQUAL_UINT32(5000, store.at(0).t_start_ms);  // filled
    TEST_ASSERT_EQUAL_UINT32(4242, store.at(1).t_start_ms);  // preserved
}

void test_stamping_producer_owned_fields_pass_through(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    bw::Event producer = makeEvent(bw::EventType::SuspectedCataplexy, 1234, 7);
    producer.confidence = 0.77f;
    producer.quality = {bw::DataQuality::Degraded, bw::DataQuality::Good};
    producer.payload[0] = 0xAB;
    producer.payload[1] = 0xCD;

    clock.setMs(1234);
    TEST_ASSERT_TRUE(logger.publish(producer).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1234));

    const bw::Event& stored = store.at(0);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SuspectedCataplexy),
                      static_cast<int>(stored.type));
    TEST_ASSERT_EQUAL_UINT32(7, stored.detector_id);  // producer's id, not 17
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.77f, stored.confidence);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::DataQuality::Degraded),
                      static_cast<int>(stored.quality.imu));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::DataQuality::Good),
                      static_cast<int>(stored.quality.ppg));
    TEST_ASSERT_EQUAL_HEX8(0xAB, stored.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, stored.payload[1]);
}

void test_stamping_t_start_unix_filled_when_clock_synced(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(1000);
    clock.setUnixSeconds(7777);  // phone has synced the wall clock

    bw::Event producer = makeEvent(bw::EventType::SleepEpisode, 0);
    TEST_ASSERT_TRUE(logger.publish(producer).ok());
    // The first drain after the wall clock becomes known also emits the
    // one-shot TimeSync record, so the drain returns 2 (SleepEpisode queued
    // first, then TimeSync) — see test_wall_time.
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(1000));
    // t_start_ms was 0 -> filled; t_start_unix was 0 -> filled from the synced
    // wall clock (§15.2 D10).
    TEST_ASSERT_EQUAL_UINT32(1000, store.at(0).t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(7777, store.at(0).t_start_unix);
}
