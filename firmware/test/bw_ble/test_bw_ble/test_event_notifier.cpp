// test_event_notifier.cpp - spec 9 row 7 (spec D7): first-subscribe
// baseline (no history burst), newest-only notifications, empty store,
// store errors skipped silently, unsubscribe stops every read.

#include <bw/ble/event_notifier.h>

#include <unity.h>

#include "ble_test_fakes.h"

using bw::Event;
using bw::ble::EventNotifier;
using bw_ble_test::FakeEventStore;
using bw_ble_test::makeEvent;

namespace {

uint32_t append(FakeEventStore<16>& store, bw::EventType type, bw::TimeMs t_ms) {
    const bw::Result<uint32_t> seq = store.append(makeEvent(0, type, t_ms));
    return seq.ok() ? seq.value : 0;
}

}  // namespace

void test_notifier_empty_store(void) {
    FakeEventStore<16> store;
    EventNotifier notifier(&store);
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));  // nothing new, honest false
}

void test_notifier_first_subscribe_baselines_no_burst(void) {
    FakeEventStore<16> store;
    append(store, bw::EventType::Boot, 1);
    append(store, bw::EventType::DrowsinessEvent, 2);
    append(store, bw::EventType::SleepEpisode, 3);  // history BEFORE subscribing

    EventNotifier notifier(&store);
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));  // no history burst (D7)

    append(store, bw::EventType::UserMarked, 200);
    TEST_ASSERT_TRUE(notifier.poll(200, out));  // only events from NOW on
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EventType::UserMarked),
                            static_cast<uint8_t>(out.type));
    TEST_ASSERT_FALSE(notifier.poll(201, out));  // delivered already
}

void test_notifier_newest_only_skips_intermediates(void) {
    FakeEventStore<16> store;
    EventNotifier notifier(&store);
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));

    append(store, bw::EventType::DrowsinessEvent, 200);     // three arrive between
    append(store, bw::EventType::DrowsinessEvent, 201);     // two ticks (spec D7:
    append(store, bw::EventType::SuspectedCataplexy, 202);  // convenience only)

    TEST_ASSERT_TRUE(notifier.poll(300, out));  // newest of the backlog wins
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EventType::SuspectedCataplexy),
                            static_cast<uint8_t>(out.type));
    TEST_ASSERT_EQUAL_UINT32(2, notifier.skippedIntermediate());  // counted, not lost silently
    TEST_ASSERT_FALSE(notifier.poll(301, out));

    append(store, bw::EventType::AlertRaised, 400);
    TEST_ASSERT_TRUE(notifier.poll(400, out));  // cursor advanced correctly
    TEST_ASSERT_EQUAL_UINT32(4, out.seq);       // seqs 1..3 were the first batch
}

void test_notifier_store_error_skips_silently(void) {
    FakeEventStore<16> store;
    EventNotifier notifier(&store);
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));  // baselines on the healthy store

    append(store, bw::EventType::Boot, 200);
    store.failReads(true);
    TEST_ASSERT_FALSE(notifier.poll(200, out));           // error: no notification...
    TEST_ASSERT_EQUAL_UINT32(1, notifier.storeErrors());  // ...but counted (spec 9/10)

    store.failReads(false);
    TEST_ASSERT_TRUE(notifier.poll(300, out));  // recovers, event still delivered
}

void test_notifier_unsubscribe_stops_reads(void) {
    FakeEventStore<16> store;
    EventNotifier notifier(&store);
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));
    append(store, bw::EventType::Boot, 200);
    TEST_ASSERT_TRUE(notifier.poll(200, out));

    notifier.unsubscribe();
    const uint32_t reads_at_unsub = store.reads();
    append(store, bw::EventType::Boot, 300);
    TEST_ASSERT_FALSE(notifier.poll(300, out));
    TEST_ASSERT_FALSE(notifier.poll(301, out));
    TEST_ASSERT_EQUAL_UINT32(reads_at_unsub, store.reads());  // zero store reads

    // Re-subscribing re-baselines: still no history burst.
    notifier.subscribe(400);
    TEST_ASSERT_FALSE(notifier.poll(400, out));
}

void test_notifier_null_store_is_honest(void) {
    EventNotifier notifier{nullptr};
    notifier.subscribe(100);
    Event out{};
    TEST_ASSERT_FALSE(notifier.poll(100, out));
    notifier.bind(nullptr);
    TEST_ASSERT_FALSE(notifier.poll(101, out));
}
