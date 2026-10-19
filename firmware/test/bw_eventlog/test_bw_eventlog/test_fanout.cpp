// spec §6 D1 + §15.2 D4: the fixed-capacity subscriber fan-out. Subscribers
// see the CANONICAL record (post-append: final seq + boot_id) in append order;
// a rejecting subscriber is counted (§10), never retried, never blocking.

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/eventlog/subscribers.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"
#include "recording_subscriber.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;
using f17_test::RecordingSubscriber;

void test_fanout_carries_final_seq_matching_store(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    TEST_ASSERT_TRUE(logger.addSubscriber(&sub));

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(100));

    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    TEST_ASSERT_EQUAL_UINT32(1, sub.count());
    TEST_ASSERT_EQUAL_UINT32(store.at(0).seq, sub.at(0).seq);  // D4: final seq, not 0
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(sub.at(0).type));
}

void test_fanout_order_matches_append_order(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);

    for (uint32_t i = 0; i < 5; ++i) {
        clock.setMs(100 + i);
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100 + i)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(5, logger.drain(200));
    TEST_ASSERT_EQUAL_UINT32(5, sub.count());
    for (size_t i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT32(store.at(i).seq, sub.at(i).seq);
        TEST_ASSERT_EQUAL_UINT32(100 + i, sub.at(i).t_start_ms);
    }
}

void test_fanout_capacity_eight_ninth_rejected(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};
    RecordingSubscriber subs[8];

    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT_TRUE(logger.addSubscriber(&subs[i]));
    }
    RecordingSubscriber ninth;
    TEST_ASSERT_FALSE(logger.addSubscriber(&ninth));  // §15.4: fixed capacity
    TEST_ASSERT_EQUAL_UINT32(8, bw::eventlog::kMaxEventSubscribers);
}

void test_fanout_null_and_duplicate_rejected(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};
    RecordingSubscriber sub;

    TEST_ASSERT_FALSE(logger.addSubscriber(nullptr));
    TEST_ASSERT_TRUE(logger.addSubscriber(&sub));
    TEST_ASSERT_FALSE(logger.addSubscriber(&sub));  // idempotent P4 double-attach
}

void test_fanout_rejecting_subscriber_counted_others_served(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber busy;     // a bounded consumer that says Err::Full
    RecordingSubscriber healthy;  // keeps up
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&busy);
    logger.addSubscriber(&healthy);
    busy.rejectNext(2);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 100)).ok());
    clock.setMs(200);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 200)).ok());

    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(200));  // rejection never blocks the drain
    TEST_ASSERT_EQUAL_UINT32(2, busy.rejected());
    TEST_ASSERT_EQUAL_UINT32(0, busy.count());
    TEST_ASSERT_EQUAL_UINT32(2, healthy.count());  // the healthy one still got both
    TEST_ASSERT_EQUAL_UINT32(2, logger.stats().subscriber_rejected);
}

void test_fanout_without_subscribers_still_appends(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);  // no addSubscriber at all (consumers optional, §3)

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(100));
    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().subscriber_rejected);
}

void test_fanout_subscriber_sees_every_drain(void) {
    // Persistent attachment: two separate drain cycles deliver both events,
    // sequence numbers continuous across the cycles.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(100));
    clock.setMs(200);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 200)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(200));

    TEST_ASSERT_EQUAL_UINT32(2, sub.count());
    TEST_ASSERT_EQUAL_UINT32(1, sub.at(0).seq);
    TEST_ASSERT_EQUAL_UINT32(2, sub.at(1).seq);
    TEST_ASSERT_EQUAL_UINT32(2, sub.calls());
}
