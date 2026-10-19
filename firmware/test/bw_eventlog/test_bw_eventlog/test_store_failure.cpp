// spec §10 + §15.2 D8: store failure behavior. A refused append leaves the
// event at the head, backs off kStoreRetryBackoffMs, reports StorageDegraded
// once per failure EPISODE — producers keep getting Ok, the overflow policy
// keeps running, and recovery preserves order and seq (no gaps).

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_fault_reporter.h>

#include "fake_event_store.h"
#include "recording_subscriber.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;
using f17_test::RecordingSubscriber;

void test_store_failure_keeps_head_reports_once(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    bw::testkit::RecordingFaultReporter faults;
    EventLogger logger{&clock, &faults};
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));

    TEST_ASSERT_EQUAL_UINT32(0, store.count());    // nothing written
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());  // the event waits (§10)
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().append_retries);
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().store_fault_episodes);
    TEST_ASSERT_EQUAL_UINT32(1, faults.countOf(bw::FaultCode::StorageDegraded));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::FaultSeverity::Warning),
                      static_cast<int>(faults.at(0).severity));
}

void test_store_failure_backoff_gates_retry(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    bw::testkit::RecordingFaultReporter faults;
    EventLogger logger{&clock, &faults};
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // attempt 1 → horizon 600
    TEST_ASSERT_EQUAL_UINT32(1, store.appendCalls());

    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // inside backoff: no attempt
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(599));  // 1 ms before the horizon
    TEST_ASSERT_EQUAL_UINT32(1, store.appendCalls());

    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(600));  // attempt 2 at the horizon
    TEST_ASSERT_EQUAL_UINT32(2, store.appendCalls());
    TEST_ASSERT_EQUAL_UINT32(2, logger.stats().append_retries);
    // Same episode → the fault is NOT re-reported per retry (D8).
    TEST_ASSERT_EQUAL_UINT32(1, faults.countOf(bw::FaultCode::StorageDegraded));
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().store_fault_episodes);
}

void test_store_failure_recovery_preserves_order_and_seq(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    bw::testkit::RecordingFaultReporter faults;
    EventLogger logger{&clock, &faults};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100)).ok());
    clock.setMs(200);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 200)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(200));  // outage: both held

    store.failAllAppends(false);                     // the log comes back
    TEST_ASSERT_EQUAL_UINT32(2, logger.drain(700));  // past the 500 ms horizon
    TEST_ASSERT_EQUAL_UINT32(2, store.count());
    TEST_ASSERT_EQUAL_UINT32(1, store.at(0).seq);  // no gap from the refusal
    TEST_ASSERT_EQUAL_UINT32(2, store.at(1).seq);
    TEST_ASSERT_EQUAL_UINT32(100, store.at(0).t_start_ms);  // FIFO preserved
    TEST_ASSERT_EQUAL_UINT32(200, store.at(1).t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(2, sub.count());  // fan-out resumes after recovery
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().store_fault_episodes);  // still one
}

void test_store_failure_new_episode_reports_again(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    bw::testkit::RecordingFaultReporter faults;
    EventLogger logger{&clock, &faults};
    logger.attachStore(&store);

    clock.setMs(100);
    store.failAllAppends(true);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // episode 1

    store.failAllAppends(false);
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(600));  // recovery ends episode 1

    store.failAllAppends(true);
    clock.setMs(2000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 2000)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(2000));  // episode 2
    TEST_ASSERT_EQUAL_UINT32(2, faults.countOf(bw::FaultCode::StorageDegraded));
    TEST_ASSERT_EQUAL_UINT32(2, logger.stats().store_fault_episodes);
}

void test_store_failure_publish_returns_ok_during_outage(void) {
    // Producers are never blocked or punished for storage trouble (§5, §10):
    // admission still succeeds while the store is down.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // establish the outage (needs an event first)
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 90)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));
    const bw::Result<void> during_outage =
        logger.publish(makeEvent(bw::EventType::SleepEpisode, 95));
    TEST_ASSERT_TRUE(during_outage.ok());
    TEST_ASSERT_EQUAL_UINT32(2, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().dropped_full);  // not a drop, just waiting
}

void test_store_failure_overflow_policy_active_during_outage(void) {
    // The outage does not turn the queue into a block: while the store is
    // down the §15.3 policy still governs the full queue.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // outage established

    for (uint32_t i = 1; i < 16; ++i) {  // fill the remaining 15 slots
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100 + i)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    // Queue full + store down: a same-priority event is still refused+counted,
    // a Critical one still evicts a Normal — exactly as without an outage.
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Full),
        static_cast<int>(logger.publish(makeEvent(bw::EventType::NapPlanned, 999)).err));
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Ok),
        static_cast<int>(logger.publish(makeEvent(bw::EventType::AlertRaised, 998)).err));
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().evicted_lower_priority);
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
}

void test_store_failure_without_reporter_is_safe(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};  // ctx.faults == nullptr (W2: no F26 yet)
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));                    // must not dereference null
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().store_fault_episodes);  // counters still honest
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());
}

void test_store_failure_full_queue_heals_in_order(void) {
    // Worst case: a full queue accumulated during an outage, then the store
    // returns — all 16 drain in FIFO order with contiguous seqs.
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    store.failAllAppends(true);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));
    for (uint32_t i = 1; i < 16; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100 + i)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    store.failAllAppends(false);
    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(600));
    TEST_ASSERT_EQUAL_UINT32(16, store.count());
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, store.at(i).seq);
        TEST_ASSERT_EQUAL_UINT32(100 + i, store.at(i).t_start_ms);
    }
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
}
