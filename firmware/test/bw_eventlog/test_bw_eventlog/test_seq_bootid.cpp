// spec §9(a): ordering and seq/boot_id monotonicity across SIMULATED reboots —
// the fake store outlives each EventLogger, so "reboot" = a fresh logger over
// the same persisted storage (§15.2 D3).

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"
#include "recording_subscriber.h"

using bw::eventlog::EventLogger;
using f17_test::FakeEventStore;
using f17_test::makeEvent;
using f17_test::RecordingSubscriber;

void test_seq_first_append_is_one_and_fanned_out(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    TEST_ASSERT_TRUE(logger.addSubscriber(&sub));

    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 1000)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));

    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    TEST_ASSERT_EQUAL_UINT32(1, store.at(0).seq);
    TEST_ASSERT_EQUAL_UINT32(1, store.at(0).boot_id);  // empty store → first boot
    TEST_ASSERT_EQUAL_UINT32(1, sub.count());
    TEST_ASSERT_EQUAL_UINT32(1, sub.at(0).seq);  // fan-out carries the final seq (D4)
    TEST_ASSERT_EQUAL_UINT32(1, sub.at(0).boot_id);
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
}

void test_seq_monotonic_in_order_across_drains(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);

    for (uint32_t i = 0; i < 5; ++i) {
        clock.setMs(1000 + i * 100);
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 1000 + i * 100)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(5, logger.drain(1500));
    TEST_ASSERT_EQUAL_UINT32(5, store.count());
    for (size_t i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, store.at(i).seq);  // 1..5, oldest first
        TEST_ASSERT_EQUAL_UINT32(1000 + i * 100, store.at(i).t_start_ms);
        TEST_ASSERT_EQUAL_UINT32(i + 1, sub.at(i).seq);  // fan-out order == append order
    }
}

void test_seq_continues_across_simulated_reboot(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;

    {
        EventLogger first_boot{&clock, nullptr};
        first_boot.attachStore(&store);
        TEST_ASSERT_TRUE(first_boot.publish(makeEvent(bw::EventType::NapPlanned, 100)).ok());
        TEST_ASSERT_TRUE(first_boot.publish(makeEvent(bw::EventType::NapPlanned, 200)).ok());
        TEST_ASSERT_EQUAL_UINT32(2, first_boot.drain(200));
        TEST_ASSERT_EQUAL_UINT32(1, first_boot.bootId());
    }  // "power off": logger destroyed, storage survives

    RecordingSubscriber sub;
    EventLogger second_boot{&clock, nullptr};
    second_boot.attachStore(&store);  // D3 recovery probe over the persisted tail
    second_boot.addSubscriber(&sub);
    TEST_ASSERT_EQUAL_UINT32(2, second_boot.bootId());  // increments each boot
    TEST_ASSERT_TRUE(second_boot.publish(makeEvent(bw::EventType::NapPlanned, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, second_boot.drain(100));

    TEST_ASSERT_EQUAL_UINT32(3, store.count());
    TEST_ASSERT_EQUAL_UINT32(3, store.at(2).seq);  // continues, never reused
    TEST_ASSERT_EQUAL_UINT32(2, store.at(2).boot_id);
    TEST_ASSERT_EQUAL_UINT32(2, sub.at(0).boot_id);
}

void test_bootid_recovers_from_preseeded_tail(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<512> store;
    for (int i = 0; i < 500; ++i) {  // a store with history: seq 1..500, boot 3
        bw::Event seed{};
        seed.type = bw::EventType::SleepEpisode;
        seed.boot_id = 3;
        seed.schema_version = bw::Event::kSchemaVersion;
        TEST_ASSERT_TRUE(store.append(seed).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(500, store.at(499).seq);

    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    TEST_ASSERT_EQUAL_UINT32(4, logger.bootId());  // tail.boot_id + 1

    clock.setMs(50);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 50)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(50));
    TEST_ASSERT_EQUAL_UINT32(501, store.at(500).seq);  // probe + append = one store
    TEST_ASSERT_EQUAL_UINT32(4, store.at(500).boot_id);
}

void test_empty_store_hint_recovers_without_read(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store, 0);
    TEST_ASSERT_EQUAL_UINT32(0, store.readCalls());
    TEST_ASSERT_EQUAL_UINT32(1, logger.bootId());
}

void test_valid_tail_hint_reads_once_and_increments_boot_id(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    for (uint32_t i = 0; i < 5; ++i) {
        bw::Event seed{};
        seed.type = bw::EventType::SleepEpisode;
        seed.boot_id = 7;
        TEST_ASSERT_TRUE(store.append(seed).ok());
    }
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store, 5);
    TEST_ASSERT_EQUAL_UINT32(1, store.readCalls());
    TEST_ASSERT_EQUAL_UINT32(8, logger.bootId());
}

void test_bad_tail_hint_falls_back_to_generic_recovery(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    for (uint32_t i = 0; i < 5; ++i) {
        bw::Event seed{};
        seed.type = bw::EventType::SleepEpisode;
        seed.boot_id = 7;
        TEST_ASSERT_TRUE(store.append(seed).ok());
    }
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store, 99);  // no event with precisely this seq
    TEST_ASSERT_TRUE(store.readCalls() > 1);
    TEST_ASSERT_EQUAL_UINT32(8, logger.bootId());
}

void test_no_hint_retains_generic_recovery(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    for (uint32_t i = 0; i < 5; ++i) {
        bw::Event seed{};
        seed.type = bw::EventType::SleepEpisode;
        seed.boot_id = 11;
        TEST_ASSERT_TRUE(store.append(seed).ok());
    }
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    TEST_ASSERT_TRUE(store.readCalls() > 1);
    TEST_ASSERT_EQUAL_UINT32(12, logger.bootId());
}

void test_failed_append_consumes_no_seq(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    store.failNextAppends(1);

    clock.setMs(100);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SensorFault, 100)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // refused → head retained
    TEST_ASSERT_EQUAL_UINT32(0, store.count());
    TEST_ASSERT_EQUAL_UINT32(1, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().append_retries);
    TEST_ASSERT_EQUAL_UINT32(1, logger.stats().store_fault_episodes);

    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(100));  // inside backoff: no attempt
    TEST_ASSERT_EQUAL_UINT32(1, store.appendCalls());

    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(600));  // exactly at horizon (100 + 500)
    TEST_ASSERT_EQUAL_UINT32(1, store.count());
    TEST_ASSERT_EQUAL_UINT32(1, store.at(0).seq);  // the refused attempt left no gap
    TEST_ASSERT_EQUAL_UINT32(0, logger.queued());
}

void test_store_absent_holds_queue_without_fanout(void) {
    bw::testkit::FakeClock clock;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.addSubscriber(&sub);  // no attachStore at all (P4 order not reached yet)

    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 100 + i)).ok());
    }
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(500));  // nothing to drain against
    TEST_ASSERT_EQUAL_UINT32(3, logger.queued());
    TEST_ASSERT_EQUAL_UINT32(0, sub.count());  // D4: no store → no fan-out
    TEST_ASSERT_EQUAL_UINT32(0, logger.stats().append_retries);
}

void test_late_store_attach_recovers_then_drains(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;

    // Persisted history from an earlier life (boot 7)...
    for (int i = 0; i < 2; ++i) {
        bw::Event seed{};
        seed.type = bw::EventType::SleepEpisode;
        seed.boot_id = 7;
        TEST_ASSERT_TRUE(store.append(seed).ok());
    }

    EventLogger logger{&clock, nullptr};
    logger.addSubscriber(&sub);
    clock.setMs(1000);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 1000, 10)).ok());
    TEST_ASSERT_EQUAL_UINT32(0, logger.drain(1000));  // no store yet: held, not lost

    logger.attachStore(&store);  // recovery runs BEFORE the first drain (D3)
    TEST_ASSERT_EQUAL_UINT32(8, logger.bootId());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(1000));

    TEST_ASSERT_EQUAL_UINT32(3, store.count());
    TEST_ASSERT_EQUAL_UINT32(3, store.at(2).seq);
    TEST_ASSERT_EQUAL_UINT32(8, store.at(2).boot_id);  // stamped post-recovery
    TEST_ASSERT_EQUAL_UINT32(8, sub.at(0).boot_id);    // same value fanned out
    TEST_ASSERT_EQUAL_UINT32(10, sub.at(0).detector_id);
}

void test_recovery_read_error_defaults_to_first_boot(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    bw::Event seed{};
    seed.type = bw::EventType::SleepEpisode;
    seed.boot_id = 5;
    TEST_ASSERT_TRUE(store.append(seed).ok());

    EventLogger logger{&clock, nullptr};
    store.failReads(1);  // first probe read fails → best-effort boot 1 (§15.7)
    logger.attachStore(&store);
    TEST_ASSERT_EQUAL_UINT32(1, logger.bootId());
}

void test_reattach_does_not_reprobe_or_skip_boot(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.setMs(10);
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 10)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(10));  // tail now exists with boot 1

    logger.attachStore(&store);  // a second attach must NOT bump the boot id
    TEST_ASSERT_EQUAL_UINT32(1, logger.bootId());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::NapPlanned, 20)).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(20));
    TEST_ASSERT_EQUAL_UINT32(2, store.at(1).seq);
    TEST_ASSERT_EQUAL_UINT32(1, store.at(1).boot_id);  // same boot, both records
}

void test_fanned_event_matches_store_record(void) {
    bw::testkit::FakeClock clock;
    FakeEventStore<> store;
    RecordingSubscriber sub;
    EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);
    logger.addSubscriber(&sub);

    bw::Event producer = makeEvent(bw::EventType::DrowsinessEvent, 4242, 7);
    producer.confidence = 0.42f;
    clock.setMs(9000);
    TEST_ASSERT_TRUE(logger.publish(producer).ok());
    TEST_ASSERT_EQUAL_UINT32(1, logger.drain(9000));

    const bw::Event& persisted = store.at(0);
    const bw::Event& notified = sub.at(0);
    TEST_ASSERT_EQUAL_UINT32(persisted.seq, notified.seq);
    TEST_ASSERT_EQUAL_UINT32(persisted.boot_id, notified.boot_id);
    TEST_ASSERT_EQUAL_UINT32(persisted.t_start_ms, notified.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(persisted.schema_version, notified.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.42f, notified.confidence);
}
