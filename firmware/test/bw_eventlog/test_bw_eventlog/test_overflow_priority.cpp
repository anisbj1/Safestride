// spec §9(b) + §6 D2 + §15.3: the priority-aware overflow policy. The queue
// admits everything while it has room; when full, a STRICTLY less important
// resident is evicted (oldest first among equals, FIFO order preserved for the
// survivors), otherwise the incoming event is refused with Err::Full — every
// path counted, Critical residents never evicted.

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"
#include "recording_subscriber.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;
using f17_test::RecordingSubscriber;

namespace {

// Fill the queue with `type` events at t = 1000, 1100, ... (unlimited types
// only — the fill itself must never trip the de-dup).
void fillQueue(EventLogger& logger, bw::EventType type, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(type, 1000 + i * 100)).ok());
    }
}

}  // namespace

void test_overflow_equal_priority_refuses_incoming(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    fillQueue(logger, bw::EventType::NapPlanned, 16);
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    const bw::Result<void> seventeenth = logger.publish(makeEvent(bw::EventType::NapPlanned, 9999));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Full), static_cast<int>(seventeenth.err));
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().evicted_lower_priority);
}

void test_overflow_rejected_event_leaves_queue_untouched(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    fillQueue(logger, bw::EventType::NapPlanned, 16);
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Full),
        static_cast<int>(logger.publish(makeEvent(bw::EventType::NapPlanned, 50)).err));
    // Same-priority incoming refused (§15.3): the queue keeps its exact FIFO
    // contents — no eviction, no reordering.
    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(2000));
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT32(1000 + i * 100, store.at(i).t_start_ms);
    }
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().evicted_lower_priority);
}

void test_overflow_critical_evicts_oldest_normal_order_preserved(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);

    fillQueue(logger, bw::EventType::NapPlanned, 16);  // t = 1000..2500
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 2600, 10)).ok());
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().evicted_lower_priority);

    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(2600));
    // The OLDEST Normal (t=1000) went; survivors keep FIFO order; the
    // incoming Critical is the NEWEST record (append order == publish order
    // minus the eviction).
    TEST_ASSERT_EQUAL_UINT32(1100, store.at(0).t_start_ms);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::NapPlanned),
                      static_cast<int>(store.at(14).type));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::AlertRaised),
                      static_cast<int>(store.at(15).type));
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, store.at(i).seq);  // no seq gap from eviction
        TEST_ASSERT_EQUAL_UINT32(i + 1, sub.at(i).seq);    // fan-out matches append
    }
}

void test_overflow_high_evicts_normal(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    fillQueue(logger, bw::EventType::NapPlanned, 16);  // Normal
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 2600)).ok());
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().evicted_lower_priority);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().dropped_full);
}

void test_overflow_normal_rejected_when_all_critical(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    for (size_t i = 0; i < 8; ++i) {  // fill with the two unlimited Critical types
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 1000 + i * 10)).ok());
        TEST_ASSERT_TRUE(
            logger.publish(makeEvent(bw::EventType::SuspectedCataplexy, 1005 + i * 10)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Full),
        static_cast<int>(logger.publish(makeEvent(bw::EventType::NapPlanned, 9999)).err));
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().evicted_lower_priority);  // nothing shed for Normal
}

void test_overflow_critical_rejected_when_queue_all_critical(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 1000 + i * 10)).ok());
        TEST_ASSERT_TRUE(
            logger.publish(makeEvent(bw::EventType::SuspectedCataplexy, 1005 + i * 10)).ok());
    }
    // Equal priority → incoming waits behind its peers' turn: refused, never
    // evicts another Critical (§15.3 pathological-storm case, visible counter).
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Full),
                      static_cast<int>(logger.publish(makeEvent(bw::EventType::Boot, 9999)).err));
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().evicted_lower_priority);
}

void test_overflow_critical_resident_survives_high_incoming(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::BatteryWarning, 900)).ok());
    fillQueue(logger, bw::EventType::NapPlanned, 15);  // full: 1 Critical + 15 Normal
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 2700)).ok());
    // Victim selection must skip the Critical resident even though it is the
    // OLDEST event: an older Normal goes instead.
    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(2700));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::BatteryWarning),
                      static_cast<int>(store.at(0).type));  // Critical still first
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(15).type));
}

void test_overflow_victim_is_oldest_of_least_important(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    // Mixed High/Normal fill (16 slots): the High resident at index 0 must be
    // spared; the victim is the OLDEST Normal, not a younger one.
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1000)).ok());  // High
    for (size_t i = 0; i < 15; ++i) {  // t = 1100..2500 Normal
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 1100 + i * 100)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DrowsinessEvent, 2600)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().evicted_lower_priority);

    FakeEventStore<> store;  // drain and check who left:
    logger.attachStore(&store);
    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(2600));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(0).type));   // the old High survived
    TEST_ASSERT_EQUAL_UINT32(1200, store.at(1).t_start_ms);  // t=1100 (oldest Normal) left
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::DrowsinessEvent),
                      static_cast<int>(store.at(15).type));
}

void test_overflow_high_water_recorded(void) {
    bw::testkit::FakeClock clock;
    EventLogger logger{&clock, nullptr};

    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().queue_high_water);
    fillQueue(logger, bw::EventType::NapPlanned, 16);
    TEST_ASSERT_EQUAL_UINT32(16, logger.stats().queue_high_water);
    TEST_ASSERT_TRUE(
        logger.publish(makeEvent(bw::EventType::AlertRaised, 9999)).ok());  // evict: count stays 16
    TEST_ASSERT_EQUAL_UINT32(16, logger.stats().queue_high_water);
}

void test_overflow_drain_frees_capacity(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    fillQueue(logger, bw::EventType::NapPlanned, 16);
    TEST_ASSERT_EQUAL(
        static_cast<int>(bw::Err::Full),
        static_cast<int>(logger.publish(makeEvent(bw::EventType::NapPlanned, 9999)).err));
    TEST_ASSERT_EQUAL_UINT32(16, logger.drain(2000));
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 3000)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(3000));
    TEST_ASSERT_EQUAL_UINT32(17, store.count());
    // Counters are cumulative: history is never rewritten by recovery.
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(16, logger.stats().queue_high_water);
}

namespace {

// audit M-1: re-enters logger.publish() from INSIDE store->append(), i.e. while
// drain() holds a peek of the head with the guard released. First call only, so
// the drain loop still terminates (a hook that fired every time would refill
// the queue one-for-one and never drain).
class HeadEvictingAppendHook final : public f17_test::AppendHook {
public:
    explicit HeadEvictingAppendHook(bw::IEventSink* sink) : sink_(sink) {}

    void onAppend() override {
        if (fired_) {
            return;
        }
        fired_ = true;
        // Critical incoming vs an all-Normal queue: the evictor's victim is
        // logical index 0 — exactly the element drain() just peeked.
        accepted_ = sink_->publish(f17_test::makeEvent(bw::EventType::AlertRaised, 9999, 10)).ok();
    }

    bool fired() const { return fired_; }
    bool accepted() const { return accepted_; }

private:
    bw::IEventSink* sink_;
    bool fired_ = false;
    bool accepted_ = false;
};

}  // namespace

void test_overflow_eviction_during_append_never_orphans_a_queued_event(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    fillQueue(logger, bw::EventType::NapPlanned, 16);  // t = 1000..2500
    TEST_ASSERT_EQUAL_UINT32(16, logger.queued());

    HeadEvictingAppendHook hook{&logger};
    store.setAppendHook(&hook);

    // One drain: it peeks the head, the hook evicts that head mid-append, the
    // append then succeeds. Without the head_epoch_ check the drain advances a
    // queue it no longer owns and orphans the element shifted into head_.
    TEST_ASSERT_EQUAL_UINT32(17, logger.drain(3000));
    store.setAppendHook(nullptr);

    TEST_ASSERT_TRUE(hook.fired());
    TEST_ASSERT_TRUE(hook.accepted());

    // Conservation (§15.4: every drop path is counted, never silent): every
    // published event is either stored or still queued — none vanishes.
    TEST_ASSERT_EQUAL_UINT32(17, store.count() + logger.queued());
    TEST_ASSERT_EQUAL_UINT32(17, store.count());
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().evicted_lower_priority);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().dropped_full);
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().append_retries);

    // The drained log is exactly the 16 fills in FIFO order plus the Critical
    // that evicted the head — the evicted head itself was still stored once.
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT32(1000 + i * 100, store.at(i).t_start_ms);
    }
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::AlertRaised),
                      static_cast<int>(store.at(16).type));
    for (size_t i = 0; i < 17; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, store.at(i).seq);  // no seq gap either
    }
}
