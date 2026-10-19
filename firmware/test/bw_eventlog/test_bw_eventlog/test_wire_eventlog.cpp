// F17 wiring sanity check (additive to §9's files): compiles wire_eventlog.cpp
// into the host suite (README §3 requires the wiring pair to build for native
// too, so P4 finds no breakage), pins the §15.4 pump constants, and drives the
// full wiring path (wire -> sink -> attachStore -> attachSubscriber -> pumpOnce)
// without FreeRTOS.

#include "../../../src/app/wiring/wire_eventlog.cpp"
#include <unity.h>

#include <bw/testkit/fake_clock.h>

#include "fake_event_store.h"
#include "recording_subscriber.h"

void test_wiring_constants_match_spec(void) {
    TEST_ASSERT_EQUAL_UINT32(100, f17::kEventPumpPeriodMs);     // §15.4 [EXPERIMENTAL]
    TEST_ASSERT_EQUAL_UINT32(2, f17::kEventPumpPriority);       // §15.4 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(3072, f17::kEventPumpStackBytes);  // §15.4 [EXPERIMENTAL]
    TEST_ASSERT_EQUAL_UINT32(1, f17::kEventPumpCore);           // runtime.md §3.2
}

void test_wiring_wire_is_safe_with_empty_context(void) {
    // Composition-root smoke test: with no clock and no fault sink, wire() must
    // construct the logger and emit Boot without dereferencing anything
    // (§10/§15.7: missing dependencies degrade, never crash).
    app::AppContext ctx{};
    f17::wire(ctx);
    TEST_ASSERT_NOT_NULL(f17::sink());
}

void test_wiring_full_path_appends_and_fans_out(void) {
    bw::testkit::FakeClock clock;
    f17_test::FakeEventStore<> store;
    f17_test::RecordingSubscriber sub;
    app::AppContext ctx{};
    ctx.clock = &clock;
    ctx.faults = nullptr;

    f17::wire(ctx);  // builds the (already-constructed) logger, emits Boot once
    f17::attachStore(&store);
    TEST_ASSERT_TRUE(f17::attachSubscriber(&sub));

    clock.setMs(1000);
    // A producer seam hands its event to the sink the composition root exposed.
    TEST_ASSERT_TRUE(
        f17::sink()->publish(f17_test::makeEvent(bw::EventType::SleepEpisode, 1000)).ok());

    f17::pumpOnce(clock.nowMs());  // Boot + SleepEpisode drain
    TEST_ASSERT_EQUAL_UINT32(2, store.count());
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(store.at(0).type));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode),
                      static_cast<int>(store.at(1).type));
    TEST_ASSERT_EQUAL_UINT32(1, store.at(0).seq);
    TEST_ASSERT_EQUAL_UINT32(2, store.at(1).seq);
    // The consumer subscriber received the canonical record with its final seq.
    TEST_ASSERT_EQUAL_UINT32(2, sub.count());
    TEST_ASSERT_EQUAL_UINT32(2, sub.at(1).seq);

    const bw::eventlog::EventLogStats snapshot = f17::stats();
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.dropped_full);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.evicted_lower_priority);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.rate_limited);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.append_retries);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.queue_high_water);
}

void test_wiring_wire_claims_pump_slot_exactly_once(void) {
    // audit L-4: wire() promises idempotence, which is only true if a repeated
    // call does NOT spawn a second drain task racing the first on one queue.
    // The claim lives ahead of the ARDUINO guard, so this host test exercises
    // the very branch the device uses; a reverted guard claims every time (or
    // never) and both readings fail this assertion.
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f17::wire(ctx);
    f17::wire(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, f17::pumpClaims());
    TEST_ASSERT_NOT_NULL(f17::sink());
}
