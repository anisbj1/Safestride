// test_link_state_tracker.cpp - spec 9 row 8 (spec D10/CR-018 + CR-019):
// Up/Down/MtuChanged/encrypted/subscription transitions, the BleLinkState
// fields on each, the "no snapshot yet" state, mtu reset on disconnect, and
// the two ISyncTransport derivations (linkUp, maxSendBytes).

#include <bw/ble/link_state_tracker.h>

#include <unity.h>

using bw::BleLinkState;
using bw::kMaxSyncFrameBytes;
using bw::TimeMs;
using bw::ble::kDefaultAttMtu;
using bw::ble::LinkEvent;
using bw::ble::LinkStateProvider;
using bw::ble::LinkStateTracker;

namespace {

LinkEvent up(TimeMs t, uint16_t mtu = kDefaultAttMtu) {
    return {LinkEvent::Kind::Up, mtu, false, t};
}
LinkEvent down(TimeMs t) {
    return {LinkEvent::Kind::Down, 0, false, t};
}
LinkEvent mtuChanged(TimeMs t, uint16_t mtu) {
    return {LinkEvent::Kind::MtuChanged, mtu, false, t};
}
LinkEvent secured(TimeMs t, bool encrypted) {
    return {LinkEvent::Kind::EncryptedChanged, 0, encrypted, t};
}
LinkEvent subscribed(TimeMs t, bool enabled) {
    return {LinkEvent::Kind::SyncDataSubscribedChanged, 0, enabled, t};
}

}  // namespace

// CR-018: "no snapshot yet" is an ordinary state - never a fabricated link.
void test_tracker_no_snapshot_before_first_event(void) {
    LinkStateTracker tracker;
    BleLinkState state{};
    TEST_ASSERT_FALSE(tracker.snapshot(state));
    TEST_ASSERT_FALSE(tracker.linkUp());
    TEST_ASSERT_EQUAL_UINT16(0, tracker.maxSendBytes());
}

void test_tracker_up_produces_unencrypted_state(void) {
    LinkStateTracker tracker;
    tracker.apply(up(500));

    BleLinkState state{};
    TEST_ASSERT_TRUE(tracker.snapshot(state));
    TEST_ASSERT_EQUAL_UINT64(500, state.t_ms);  // stamped when PRODUCED (CR-018)
    TEST_ASSERT_TRUE(state.connected);
    TEST_ASSERT_FALSE(state.encrypted);  // a fresh link starts unencrypted (D1)
    TEST_ASSERT_EQUAL_UINT16(kDefaultAttMtu, state.mtu);
    TEST_ASSERT_FALSE(tracker.linkUp());  // not yet encrypted/subscribed
}

void test_tracker_encryption_and_subscription_gates_linkup(void) {
    LinkStateTracker tracker;
    tracker.apply(up(500));

    tracker.apply(secured(600, true));
    BleLinkState state{};
    TEST_ASSERT_TRUE(tracker.snapshot(state));
    TEST_ASSERT_TRUE(state.encrypted);
    TEST_ASSERT_FALSE(tracker.linkUp());  // still no Sync Data subscription

    tracker.apply(subscribed(700, true));
    TEST_ASSERT_TRUE(tracker.linkUp());  // connected + encrypted + subscribed (CR-019)
    TEST_ASSERT_TRUE(tracker.snapshot(state));
    TEST_ASSERT_EQUAL_UINT64(700, state.t_ms);

    tracker.apply(subscribed(800, false));  // peer disabled notifications
    TEST_ASSERT_FALSE(tracker.linkUp());
}

void test_tracker_mtu_transitions(void) {
    LinkStateTracker tracker;
    tracker.apply(up(500));
    TEST_ASSERT_EQUAL_UINT16(kDefaultAttMtu - 3, tracker.maxSendBytes());  // 20

    tracker.apply(mtuChanged(600, 185));  // preferred MTU (checklist 4)
    BleLinkState state{};
    TEST_ASSERT_TRUE(tracker.snapshot(state));
    TEST_ASSERT_EQUAL_UINT16(185, state.mtu);
    TEST_ASSERT_EQUAL_UINT16(182, tracker.maxSendBytes());

    tracker.apply(mtuChanged(700, 255));                                   // stack ceiling
    TEST_ASSERT_EQUAL_UINT16(kMaxSyncFrameBytes, tracker.maxSendBytes());  // 252
}

void test_tracker_down_resets_per_link_state(void) {
    LinkStateTracker tracker;
    tracker.apply(up(500));
    tracker.apply(secured(550, true));
    tracker.apply(subscribed(560, true));
    tracker.apply(mtuChanged(570, 185));
    TEST_ASSERT_TRUE(tracker.linkUp());

    tracker.apply(down(900));
    BleLinkState state{};
    TEST_ASSERT_TRUE(tracker.snapshot(state));  // the snapshot survives: connected=false
    TEST_ASSERT_FALSE(state.connected);
    TEST_ASSERT_FALSE(state.encrypted);
    TEST_ASSERT_EQUAL_UINT16(kDefaultAttMtu, state.mtu);  // resets (spec 9)
    TEST_ASSERT_EQUAL_UINT64(900, state.t_ms);
    TEST_ASSERT_FALSE(tracker.linkUp());
    TEST_ASSERT_EQUAL_UINT16(0, tracker.maxSendBytes());
}

void test_tracker_second_link_starts_clean(void) {
    LinkStateTracker tracker;
    tracker.apply(up(500));
    tracker.apply(secured(550, true));
    tracker.apply(subscribed(560, true));
    tracker.apply(down(900));
    tracker.apply(up(1000, 185));  // new phone: subscriptions/encryption gone

    BleLinkState state{};
    TEST_ASSERT_TRUE(tracker.snapshot(state));
    TEST_ASSERT_TRUE(state.connected);
    TEST_ASSERT_FALSE(state.encrypted);
    TEST_ASSERT_EQUAL_UINT16(185, state.mtu);  // initial MTU of the new link
    TEST_ASSERT_FALSE(tracker.linkUp());
}

void test_link_state_provider_over_mailbox(void) {
    LinkStateProvider provider;  // nothing bound: honest "no data yet"
    BleLinkState state{};
    TEST_ASSERT_FALSE(provider.latest(state));

    bw::LatestValue<bw::BleLinkState> mailbox;
    LinkStateTracker tracker;
    provider.bind(&mailbox);
    TEST_ASSERT_FALSE(provider.latest(state));  // bound but nothing produced

    tracker.apply(up(42));
    BleLinkState produced{};
    TEST_ASSERT_TRUE(tracker.snapshot(produced));
    mailbox.set(produced);  // exactly what ConnectionMonitor does

    TEST_ASSERT_TRUE(provider.latest(state));
    TEST_ASSERT_TRUE(state.connected);
    TEST_ASSERT_EQUAL_UINT64(42, state.t_ms);
}
