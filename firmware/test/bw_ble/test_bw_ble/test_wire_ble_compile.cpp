// bw_ble - wiring compile + seam test (spec 18, DoD 14/H7).
//
// wire_ble.cpp is compiled for the HOST by including it into this translation
// unit (same-TU include, so the f19:: statics are observable -
// test_wire_confirm_compile.cpp precedent). On the host there is no radio:
// wire() runs the same binding pass as the device but constructs nothing,
// no task is created, and every getter is an honest nullptr - the same
// state F20's host tests rely on (CR-019: they use a fake transport).
// spec D12: the transport->link binding inside that pass is host-EXECUTED
// and pinned below (H10 revert guard for M4 bench bug 1).

#include "../../../src/app/wiring/wire_ble.cpp"

#include <unity.h>

#include "ble_test_fakes.h"

using bw_ble_test::FakeSettings;
using bw_ble_test::FakeSyncLink;

// Order-dependent by design: a single shared f19:: static state, exactly as
// the composition root sees it (registered in this order in main.cpp).
void test_wire_getters_null_before_wire(void) {
    TEST_ASSERT_NULL(f19::syncTransport());
    TEST_ASSERT_NULL(f19::linkProvider());
    TEST_ASSERT_NULL(f19::server());
}

void test_wire_idempotent_and_null_seams(void) {
    app::AppContext ctx{};  // every pointer default-null: the honest empty context
    f19::wire(ctx);
    f19::wire(ctx);  // idempotent: no second task, no re-init

    // Host build: no BLE stack exists here, the getters stay null forever.
    TEST_ASSERT_NULL(f19::syncTransport());
    TEST_ASSERT_NULL(f19::linkProvider());
    TEST_ASSERT_NULL(f19::server());

    // The attach seams must survive null arguments without crashing.
    f19::attachProviders(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    f19::attachIdentity(nullptr, nullptr);

    FakeSettings settings;
    settings.add(static_cast<bw::SettingKey>(1), bw::SettingType::Bool,
                 FakeSettings::makeBool(false), FakeSettings::makeBool(true),
                 FakeSettings::makeBool(false), "alert_enabled");
    f19::attachProviders(nullptr, nullptr, nullptr, &settings, nullptr, nullptr);
    f19::attachIdentity("rev-a", "ev1;g1");
}

// The task constants the G1 ruling pinned (runtime.md 3.2 registry row).
void test_wire_task_constants_pinned(void) {
    TEST_ASSERT_EQUAL_UINT32(4096, f19::kCommsTaskStackBytes);
    TEST_ASSERT_EQUAL_UINT32(2, f19::kCommsTaskPriority);
    TEST_ASSERT_EQUAL_UINT32(0, f19::kCommsTaskCore);
}

// M4 bench bug 1 revert guard (H10, spec D12 / backlog 36): the binding
// pass applyBindings() runs - on the DEVICE AND HERE - must connect the
// transport to the link slot. If that call ever disappears, linkUp() is
// false on every device, F20 can never send, and this test fails on the
// host. The device sets the slot to &s_monitor in wire(); the host has no
// radio, so the test swaps a fake into the same slot and re-runs the pass
// exactly like the attach* seams do.
void test_wire_transport_binds_link_slot(void) {
    // After wire(): bound, but no link source exists on the host -
    // honest "no link" in every direction.
    TEST_ASSERT_FALSE(f19::s_transport.linkUp());
    TEST_ASSERT_EQUAL_UINT16(0, f19::s_transport.maxSendBytes());

    FakeSyncLink link;
    f19::s_link = &link;   // what wire() does on the device (&s_monitor)
    f19::applyBindings();  // the same re-bind pass wire()/attach* seams run

    TEST_ASSERT_FALSE(f19::s_transport.linkUp());  // bound, not usable yet

    link.setLink(true, 182);
    TEST_ASSERT_TRUE(f19::s_transport.linkUp());
    TEST_ASSERT_EQUAL_UINT16(182, f19::s_transport.maxSendBytes());

    link.setLink(false, 0);
    TEST_ASSERT_FALSE(f19::s_transport.linkUp());
    TEST_ASSERT_EQUAL_UINT16(0, f19::s_transport.maxSendBytes());

    // send attempts follow the gate through the wiring path too: refused
    // with the documented Err::NotReady while down; while up the hand-off
    // runs (host base = Unsupported, never NotReady - the gate opened).
    uint8_t frame[4] = {1, 2, 3, 4};
    bw::Result<void> r = f19::s_transport.send(bw::SyncChannel::Control, frame, sizeof(frame));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_TRUE(r.err == bw::Err::NotReady);

    link.setLink(true, 182);
    r = f19::s_transport.send(bw::SyncChannel::Control, frame, sizeof(frame));
    TEST_ASSERT_TRUE(r.err != bw::Err::NotReady);
}
