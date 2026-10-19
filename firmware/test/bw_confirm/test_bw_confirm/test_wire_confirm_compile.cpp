// bw_confirm — wiring compile + seam test (plan §15, DoD §14).
//
// wire_confirm.cpp is compiled for the host by including it into this
// translation unit (same-TU include, so its file-static manager and the f14::
// bindings are observable — test_wire_diag_compile.cpp precedent). No task is
// created (plan §15), so nothing ARDUINO-guarded needs excluding here.
//
// NOTE: f14::manager() is a static singleton shared by these four tests —
// they are ORDER-DEPENDENT and registered in main.cpp in exactly this order.

#include "../../../src/app/wiring/wire_confirm.cpp"

#include <unity.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

void test_wire_manager_reachable(void) {
    bw::confirm::ConfirmationManager& mgr = f14::manager();
    TEST_ASSERT_NOT_NULL(&mgr);
    bw::ConfirmPromptState s{};
    TEST_ASSERT_FALSE(mgr.latest(s));  // no step() yet anywhere
}

// No clock yet: honest wait (F09 pump rule) — no fabricated timeline, no
// provider state, no crash (AppContext::clock defaults to nullptr).
void test_wire_step_null_clock_waits_honestly(void) {
    app::AppContext ctx{};
    f14::wire(ctx);
    f14::step(ctx);
    bw::ConfirmPromptState s{};
    TEST_ASSERT_FALSE(f14::manager().latest(s));
}

// The D3 ack seam: step(ctx) pulls AlertStatus out of the app-owned mailbox,
// and the defer/offer timeline runs off ctx.clock — no F10 header included.
void test_wire_step_reads_alert_status_mailbox(void) {
    app::AppContext ctx{};
    FakeClock clock;
    ctx.clock = &clock;
    ctx.alert_status.set(makeAlert(true, 500));  // ack edge → defer until 5500

    f14::wire(ctx);
    f14::onEvent(makeEvent(7, bw::EventType::DrowsinessEvent, 400));

    clock.set(5499);
    f14::step(ctx);
    bw::ConfirmPromptState s{};
    TEST_ASSERT_TRUE(f14::manager().latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(s.phase));

    clock.set(5500);
    f14::step(ctx);
    TEST_ASSERT_TRUE(f14::manager().latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(7, s.seq);
    TEST_ASSERT_EQUAL_UINT64(5500 + bw::confirm::kPromptTimeoutMs, s.offer_deadline_ms);
}

// The remaining P4 seams: attach slots for F17/F18 and the command route.
void test_wire_seams_forward_to_manager(void) {
    FakeEventStore store;
    FakeEventSink sink;
    f14::attachEventStore(&store);
    f14::attachEventSink(&sink);

    TEST_ASSERT_TRUE(f14::onCommand(makeConfirm(7, 6000)));  // offered in the test above
    const LabelCall* call = store.find(7);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(call->label));
    TEST_ASSERT_EQUAL_UINT(1, sink.count());  // D5 notification out

    app::AppContext ctx{};
    f14::wire(ctx);  // idempotent, construction only
    TEST_ASSERT_EQUAL_UINT32(1, f14::manager().labelStats().confirmed);
}
