// bw_sync — wiring seam (F20 §9 W01–W02, F08/F12 pattern): wire_sync.cpp is
// COMPILED for the host by including it into this translation unit; the test
// then drives the real path through f20::pumpOnce with a fake transport. The
// composition root (app.cpp, application-level) is NOT touched here: it will
// call f20::wire / f20::pumpOnce / f20::statusProvider at P4.

#include "../../../src/app/wiring/wire_sync.cpp"

#include <unity.h>

#include <bw/testkit/mem_event_store.h>

#include "support/lossy_transport_fake.h"
#include "support/phone_harness.h"
#include "support/sync_fixtures.h"

using f20test::LossyTransportFake;
using f20test::PhoneHarness;

void test_wire_pump_once_drives_real_path(void) {
    static bw::testkit::InMemoryEventStore<> store;  // static: f20's singleton binds pointers
    static LossyTransportFake fake;
    static PhoneHarness harness(fake);

    store.clear();
    fake.reset();
    f20test::fillStore(store, 7);

    f20::wire(&fake, &store);
    fake.setLinkUp(true);
    bw::TimeMs now = 0;

    // Pump through the WIRING seam, not the engine: the composition root's cadence.
    harness.requestFrom(1);
    for (int i = 0; i < 10; ++i) {
        f20::pumpOnce(now);
        ++now;
        harness.drainAndAck();
    }
    TEST_ASSERT_EQUAL_UINT(7, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(7, store.lastSyncedSeq());

    // The ISyncStatusProvider getter (P4 seam) serves a snapshot.
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(f20::statusProvider().latest(status));
    TEST_ASSERT_EQUAL_UINT32(7, status.last_synced_seq);
    TEST_ASSERT_EQUAL_UINT16(bw::sync::kSyncProtocolVersion, status.protocol_version);
    TEST_ASSERT_BITS_HIGH(bw::sync::kStatusFlagLinkUp, status.flags);

    f20::wire(nullptr, nullptr);  // detach: no crash, honest no-op
    f20::pumpOnce(now);
}

void test_wire_rewire_binds_second_context(void) {
    static bw::testkit::InMemoryEventStore<> store_a;
    static bw::testkit::InMemoryEventStore<> store_b;
    static LossyTransportFake fake_a;
    static LossyTransportFake fake_b;
    static PhoneHarness harness_a(fake_a);
    static PhoneHarness harness_b(fake_b);

    store_a.clear();
    store_b.clear();
    fake_a.reset();
    fake_b.reset();
    f20test::fillStore(store_a, 3);
    f20test::fillStore(store_b, 5);
    bw::TimeMs now = 100;

    // Context A syncs its 3 events.
    f20::wire(&fake_a, &store_a);
    fake_a.setLinkUp(true);
    harness_a.requestFrom(1);
    for (int i = 0; i < 10; ++i) {
        f20::pumpOnce(now);
        ++now;
        harness_a.drainAndAck();
    }
    TEST_ASSERT_EQUAL_UINT32(3, store_a.lastSyncedSeq());

    // Rewire to context B: the SAME static engine must drive B only.
    f20::wire(&fake_b, &store_b);
    fake_b.setLinkUp(true);
    harness_b.requestFrom(1);
    for (int i = 0; i < 10; ++i) {
        f20::pumpOnce(now);
        ++now;
        harness_b.drainAndAck();
    }
    TEST_ASSERT_EQUAL_UINT(5, harness_b.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(5, store_b.lastSyncedSeq());

    // A is untouched by B's traffic; the provider reflects the live binding.
    TEST_ASSERT_EQUAL_UINT32(3, store_a.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(3, harness_a.storedSeqs().size());
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(f20::statusProvider().latest(status));
    TEST_ASSERT_EQUAL_UINT32(5, status.last_synced_seq);

    f20::wire(nullptr, nullptr);
}
