// bw_sync — fault-injection matrix (F20 §9 F01–F12) and consistency properties
// (P01–P04). Scripted faults only, no randomness: every failure is injected at
// an exact point (drop/duplicate/reorder/Busy/send-fail on the transport,
// readFrom failure on the store, garbage on the wire) and the run is driven to
// quiescence. The M4 exit criterion is F04: disconnect mid-sync resumes from
// the last cumulative ack with NO gap and NO duplicate store write.

#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include <bw/sync/sync_config.h>
#include <bw/sync/sync_session.h>
#include <bw/testkit/mem_event_store.h>

#include "support/failing_event_store.h"
#include "support/lossy_transport_fake.h"
#include "support/phone_harness.h"
#include "support/sync_fixtures.h"

using bw::sync::SyncPhase;
using bw::sync::SyncSession;
using f20test::LossyTransportFake;
using f20test::PhoneHarness;
namespace k = bw::sync;

// -- F01 dropped chunk ------------------------------------------------------------

void test_fault_dropped_chunk_resends_without_loss(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 12);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);  // link-up STATUS
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    fake.dropNextData();  // chunk #1 is accepted, then lost on the air
    (void)session.step(now);
    ++now;
    (void)harness.drainAndAck();  // nothing arrived: no ack, marker still 0
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());

    // kAckTimeoutMs later the unacked suffix is resent (same chunk_no, §4.2).
    now += k::kAckTimeoutMs;
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(12, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(12, harness.storeWrites());  // zero loss, zero duplicates
    TEST_ASSERT_EQUAL_UINT32(12, store.lastSyncedSeq());
    TEST_ASSERT_TRUE(session.chunksResent() >= 1);
}

// -- F02 duplicated chunk -----------------------------------------------------------

void test_fault_duplicated_chunk_is_idempotent(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 8);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    fake.duplicateNextData();  // chunk #1 delivered twice
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(8, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(8, harness.storeWrites());  // duplicates never re-write
    TEST_ASSERT_TRUE(harness.duplicateDeliveries() > 0);
    TEST_ASSERT_EQUAL_UINT32(8, store.lastSyncedSeq());  // marker advanced once
}

// -- F03 reordered chunks -------------------------------------------------------------

void test_fault_reordered_chunks_assemble_by_seq(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 10);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    fake.holdNextData();  // chunk #1 arrives after chunk #2
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(10, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(10, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(10, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(0, harness.corruptChunks());
}

// -- F04 disconnect mid-sync (M4 exit criterion) ------------------------------------------

void test_fault_disconnect_mid_sync_resumes_with_no_gap_no_dup(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 20);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    harness.setSuppressAcks(true);  // keep the transfer mid-flight
    (void)session.step(now);        // chunks 5+5+5+1 = events 1..16 in flight
    ++now;
    (void)harness.drain();  // companion committed 1..16, acks not written yet
    TEST_ASSERT_EQUAL_UINT(16, harness.storedSeqs().size());
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::WaitAck);

    // Disconnect mid-sync: marker unchanged, transfer aborted.
    fake.setLinkUp(false);
    (void)session.step(now);
    ++now;
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::Idle);

    // Reconnect: the phone resumes from its own committed high-water + 1.
    fake.setLinkUp(true);
    harness.setSuppressAcks(false);
    (void)session.step(now);  // link-up STATUS
    ++now;
    (void)harness.drain();
    TEST_ASSERT_EQUAL_UINT(17, harness.highWater() + 1);
    TEST_ASSERT_TRUE(harness.requestFrom(harness.highWater() + 1));
    f20test::runToQuiescence(session, fake, harness, now);

    // No gap (companion set == source set, each exactly once), no duplicate
    // store write, marker ends at the head.
    TEST_ASSERT_EQUAL_UINT(20, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(20, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT(0, harness.storedSeqs().count(0));
    TEST_ASSERT_EQUAL_UINT(1, harness.storedSeqs().count(20));
    TEST_ASSERT_EQUAL_UINT32(20, store.lastSyncedSeq());
}

// -- F05 link flap --------------------------------------------------------------------------

void test_fault_link_flap_twice_converges(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 14);

    bw::TimeMs now = 0;
    for (int flap = 0; flap < 2; ++flap) {
        fake.setLinkUp(true);
        (void)session.step(now);
        ++now;
        (void)harness.drain();
        TEST_ASSERT_TRUE(harness.requestFrom(harness.highWater() + 1));
        (void)session.step(now);  // stream one round
        ++now;
        (void)harness.drainAndAck();
        fake.setLinkUp(false);  // flap down mid-run
        (void)session.step(now);
        ++now;
    }
    // Final stable link: the phone re-requests and everything converges.
    fake.setLinkUp(true);
    (void)session.step(now);
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.requestFrom(harness.highWater() + 1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(14, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(14, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(14, store.lastSyncedSeq());
}

// -- F06 reboot mid-sync ---------------------------------------------------------------------

void test_fault_device_reboot_mid_sync_no_loss_no_dup(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    f20test::fillStore(store, 18);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    {
        SyncSession first_boot;
        first_boot.bind(&fake, &store);
        harness.setSuppressAcks(true);
        TEST_ASSERT_TRUE(harness.requestFrom(1));
        (void)first_boot.step(now);  // chunks in flight…
        ++now;
        (void)harness.drain();
        // …power cut: the engine's state is gone, the store (and its marker) persists.
    }
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());

    SyncSession second_boot;  // fresh engine, same persisted store (F06)
    second_boot.bind(&fake, &store);
    harness.setSuppressAcks(false);
    (void)second_boot.step(now);  // link-up STATUS (link stayed up)
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.requestFrom(harness.highWater() + 1));
    f20test::runToQuiescence(second_boot, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(18, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(18, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(18, store.lastSyncedSeq());
}

// -- F-5 / §4.6 companion reboot
// ---------------------------------------------------------------------

void test_fault_companion_reboot_mid_transfer_resumes_from_persisted_hw(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 20);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    harness.setSuppressAcks(true);  // acks are lost while the phone is going down
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(16, harness.storedSeqs().size());  // 4 chunks in flight
    TEST_ASSERT_EQUAL_UINT32(16, harness.highWater());        // committed 1..16

    // Power cut on the phone: the in-RAM view is lost, the PERSISTED
    // high-water survives and IS the resume point (SYNC_PROTOCOL.md §4.6).
    harness.simulateCompanionReboot();
    TEST_ASSERT_EQUAL_UINT32(16, harness.highWater());       // hw_ survives the reboot
    TEST_ASSERT_EQUAL_UINT(0, harness.storedSeqs().size());  // observed set gone

    // Phone is back: its first ack carries the persisted hw, the in-flight
    // window slides and the transfer converges — zero loss, zero dup commits.
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(20, harness.highWater());
    TEST_ASSERT_EQUAL_UINT32(20, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(20, harness.storeWrites());  // every event committed exactly once
    TEST_ASSERT_EQUAL_UINT(0, harness.duplicateDeliveries());
    // The post-reboot RAM view holds exactly the remainder it re-saw; events
    // 1..16 are proven durable by hw_ == 16 at the reboot (§4.6), not re-seen.
    TEST_ASSERT_EQUAL_UINT(4, harness.storedSeqs().size());
}

// -- F07 persistent send failure ----------------------------------------------------------------

void test_fault_persistent_send_failure_aborts_bounded(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 6);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    fake.failNextSend(bw::Err::Timeout, 1000);  // the radio is wedged
    for (int i = 0; i < 5; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
    }
    // Aborted after kMaxAckTimeoutsPerTransfer consecutive failures; marker untouched.
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::Idle);
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(session.latest(status));
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagTransferAborted, status.flags);

    // The phone re-requests once the radio is healthy again (§4.7).
    fake.failNextSend(bw::Err::Ok, 0);
    // With the radio healthy again the owed STATUS goes out: the RECEIVER must
    // observe transfer_aborted in the parsed frame (audit F-2a) — pump once
    // BEFORE re-requesting, while aborted_ is still latched.
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagTransferAborted, harness.lastStatusFlags());
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, harness.lastStatusFlags());
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(6, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());
}

// -- F08 lost ack -------------------------------------------------------------------------------

void test_fault_lost_ack_timeout_resend_converges(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 5);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);  // link-up STATUS
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    harness.setSuppressAcks(true);  // the companion's ack never reaches the device
    (void)session.step(now);
    ++now;
    (void)harness.drain();  // committed 1..5, ack lost
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());

    now += k::kAckTimeoutMs;  // device resends the unacked chunk (same chunk_no)
    harness.setSuppressAcks(false);
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(5, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(5, harness.storeWrites());  // resend was a no-op upsert
    TEST_ASSERT_EQUAL_UINT(5, harness.duplicateDeliveries());
    TEST_ASSERT_EQUAL_UINT32(5, store.lastSyncedSeq());
    TEST_ASSERT_TRUE(session.chunksResent() >= 1);
    // V5 transcript fixture (SYNC_PROTOCOL.md §6) — regenerate by running this
    // test; the format is documented there.
    for (const std::string& l : harness.transcript()) {
        TEST_MESSAGE(l.c_str());
    }
}

// -- F09 Err::Busy backpressure
// --------------------------------------------------------------------

void test_fault_busy_send_retried_next_pump(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 6);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    fake.failNextSend(bw::Err::Busy, 1);  // first chunk hits congestion
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());  // nothing consumed
    TEST_ASSERT_EQUAL_UINT(0, store.lastSyncedSeq());

    f20test::runToQuiescence(session, fake, harness, now);  // retried, no loss
    TEST_ASSERT_EQUAL_UINT(6, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(6, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());
}

// -- F10 MTU below floor
// ------------------------------------------------------------------------------

void test_fault_mtu_below_floor_sends_nothing(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 6);

    fake.setLinkUp(true);
    fake.setUsableBytes(51);  // MTU 54: one byte below the 52 usable floor
    bw::TimeMs now = 0;
    (void)session.step(now);
    ++now;
    (void)harness.drain();
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(session.latest(status));
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagMtuTooLow, status.flags);
    // Receiver side (audit F-2a): the phone must see the same bit in the
    // STATUS frame it actually parsed, not only in the provider snapshot.
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, harness.lastStatusFlags());
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagMtuTooLow, harness.lastStatusFlags());

    TEST_ASSERT_TRUE(harness.requestFrom(1));
    for (int i = 0; i < 3; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
    }
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());  // status flag only, no chunks
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
    TEST_ASSERT_TRUE(harness.statusFrames() >= 1);

    // MTU recovers: the phone re-requests and sync completes normally.
    fake.setUsableBytes(252);
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(6, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());
}

// -- F11 store read failure
// ----------------------------------------------------------------------------

void test_fault_store_read_failure_abandons_then_retries(void) {
    bw::testkit::InMemoryEventStore<> inner;
    f20test::FailingEventStore store(inner);
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 9);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    store.failReadAt(store.readCalls() + 1);  // the next readFrom fails
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::pumpAndDrain(session, fake, harness, now);
    store.clearFault();

    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());  // attempt abandoned
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());    // marker unchanged
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(session.latest(status));
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagTransferAborted, status.flags);
    // Receiver side (audit F-2a): the abort also travels in the parsed frame.
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagTransferAborted, harness.lastStatusFlags());

    // Retried later (phone re-requests): completes with no loss/dup.
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(9, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(9, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(9, store.lastSyncedSeq());
}

// -- F12 inbound garbage
// ---------------------------------------------------------------------------------

void test_fault_inbound_garbage_dropped_session_healthy(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 4);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);
    ++now;
    (void)harness.drain();

    // Garbage: unknown version, unknown type, short frame, wrong channel, junk.
    uint8_t junk[12] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, junk, sizeof junk));
    uint8_t short_frame[5] = {1, 1, 0, 0, 0};
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, short_frame, sizeof short_frame));
    uint8_t valid[k::kCursorFrameBytes] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Data, valid, sizeof valid));
    f20test::pumpAndDrain(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(3, session.framesDropped());  // counted, never a crash
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());

    // The session stays healthy: a valid request still syncs everything.
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(4, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(4, store.lastSyncedSeq());
}

// -- P01 idempotent replay
// -------------------------------------------------------------------------------

void test_property_idempotent_replay(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 20);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(20, harness.storeWrites());
    TEST_ASSERT_EQUAL_UINT32(20, store.lastSyncedSeq());

    // Replay the WHOLE transfer: the phone re-requests from 1 (e.g. its DB was
    // wiped but the device marker persists) — upserts are exact no-ops.
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(20, harness.storedSeqs().size());  // identical set, size N
    TEST_ASSERT_EQUAL_UINT(20, harness.storeWrites());        // no second write
    TEST_ASSERT_EQUAL_UINT32(20, store.lastSyncedSeq());
}

// -- P02/P03/P04 fault-matrix invariants
// ------------------------------------------------------------------ The matrix: a SCRIPT TABLE
// over every delivery fault the fake can place deterministically — none / drop / duplicate /
// reorder(hold) — at the nth Data send for every n the initial pass produces (audit F-6 widened
// this from drop-only to the documented space). Invariants: marker monotonic (P02), zero loss and
// zero duplicates (P03), marker never ahead of the companion's committed high-water (P04).

namespace {

// [DERIVED] Scenario size: 20 events, 5 records/chunk, 16 read per pump ⇒ the
// initial pass always emits 4 Data sends (5+5+5+1) plus one more (17..20)
// once the first ack slides the window — 5 Data sends total. Scripting
// nth = 1..kMatrixMaxNth therefore covers every nth position on a run's
// first pass. Why 20 events: one full flow-control window (4×5) + a
// remainder, the smallest store that exercises window slides and refills.
constexpr std::size_t kMatrixEvents = 20;
constexpr std::size_t kMatrixMaxNth = 5;

enum class ScriptKind : std::uint8_t { None = 0, Drop = 1, Duplicate = 2, Reorder = 3 };

struct MatrixResult {
    std::size_t store_writes = 0;
    std::size_t stored = 0;
    std::size_t drops = 0;
    std::size_t duplicates = 0;
    std::size_t reorders = 0;
    bool monotonic = true;
    bool marker_ahead = false;
    bool request_ok = false;
    bool converged = false;
};

// Runs one scripted scenario (kind == None ⇒ no fault, nth ignored).
MatrixResult runScriptScenario(ScriptKind kind, std::size_t nth) {
    MatrixResult result;
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, kMatrixEvents);

    fake.setLinkUp(true);
    switch (kind) {
    case ScriptKind::Drop:
        fake.dropDataFrame(nth);
        break;
    case ScriptKind::Duplicate:
        fake.duplicateDataFrame(nth);
        break;
    case ScriptKind::Reorder:
        fake.holdDataFrame(nth);
        break;
    case ScriptKind::None:
        break;
    }
    bw::TimeMs now = 0;
    uint32_t marker_before = 0;
    result.request_ok = harness.requestFrom(1);
    for (int i = 0; i < 40; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
        const uint32_t marker = store.lastSyncedSeq();
        if (marker < marker_before) {
            result.monotonic = false;  // P02: the marker never decreases
        }
        if (marker > harness.highWater()) {
            result.marker_ahead = true;  // P04: never ahead of the companion
        }
        marker_before = marker;
        if (harness.storedSeqs().size() == kMatrixEvents && marker == kMatrixEvents &&
            fake.phoneFramesPending() == 0) {
            result.converged = true;
            break;  // converged
        }
        now += k::kAckTimeoutMs / 4;  // resends fire if an ack was lost
    }
    result.store_writes = harness.storeWrites();
    result.stored = harness.storedSeqs().size();
    result.drops = fake.droppedByFault();
    result.duplicates = harness.duplicateDeliveries();
    result.reorders = fake.reorderedByFault();
    return result;
}

struct Script {
    ScriptKind kind;
    std::size_t nth;  // 1-based Data-send index; unused (0) for None
};

// [DERIVED] The enumerated matrix: 1 clean run + 3 fault kinds ×
// kMatrixMaxNth positions = 16 deterministic scripts (no randomness), so the
// table is static. static_assert pins count and array size together.
constexpr std::size_t kMatrixScriptCount = 1 + 3 * kMatrixMaxNth;
constexpr Script kMatrixScripts[kMatrixScriptCount] = {
    {ScriptKind::None, 0},      {ScriptKind::Drop, 1},      {ScriptKind::Drop, 2},
    {ScriptKind::Drop, 3},      {ScriptKind::Drop, 4},      {ScriptKind::Drop, 5},
    {ScriptKind::Duplicate, 1}, {ScriptKind::Duplicate, 2}, {ScriptKind::Duplicate, 3},
    {ScriptKind::Duplicate, 4}, {ScriptKind::Duplicate, 5}, {ScriptKind::Reorder, 1},
    {ScriptKind::Reorder, 2},   {ScriptKind::Reorder, 3},   {ScriptKind::Reorder, 4},
    {ScriptKind::Reorder, 5},
};

// Identifies the failing script in every assertion message (host test only).
inline std::string scriptWhere(std::size_t idx, const Script& script) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "script #%zu (kind=%u nth=%zu): ", idx,
                  static_cast<unsigned>(script.kind), script.nth);
    return std::string(buf);
}

}  // namespace

void test_property_marker_monotonic_across_matrix(void) {
    for (std::size_t idx = 0; idx < kMatrixScriptCount; ++idx) {
        const Script& script = kMatrixScripts[idx];
        const MatrixResult r = runScriptScenario(script.kind, script.nth);
        const std::string where = scriptWhere(idx, script);
        TEST_ASSERT_TRUE_MESSAGE(r.request_ok, (where + "request write failed").c_str());
        TEST_ASSERT_TRUE_MESSAGE(r.converged, (where + "scenario did not converge").c_str());
        TEST_ASSERT_TRUE_MESSAGE(r.monotonic, (where + "marker decreased (P02)").c_str());
        TEST_ASSERT_TRUE_MESSAGE(!r.marker_ahead,
                                 (where + "marker ahead of companion (P04)").c_str());
    }
}

void test_property_zero_loss_zero_duplicates_across_matrix(void) {
    for (std::size_t idx = 0; idx < kMatrixScriptCount; ++idx) {
        const Script& script = kMatrixScripts[idx];
        const MatrixResult r = runScriptScenario(script.kind, script.nth);
        const std::string where = scriptWhere(idx, script);
        TEST_ASSERT_TRUE_MESSAGE(r.converged, (where + "scenario did not converge").c_str());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(kMatrixEvents, r.stored,
                                       (where + "lost events (P03)").c_str());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(kMatrixEvents, r.store_writes,
                                       (where + "duplicate writes (P03)").c_str());
        // The scripted fault must actually fire — the matrix is not vacuous.
        switch (script.kind) {
        case ScriptKind::None:
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, r.drops,
                                           (where + "clean run dropped a frame").c_str());
            break;
        case ScriptKind::Drop:
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1, r.drops,
                                           (where + "drop did not fire exactly once").c_str());
            break;
        case ScriptKind::Duplicate:
            TEST_ASSERT_TRUE_MESSAGE(r.duplicates > 0, (where + "duplicate did not fire").c_str());
            break;
        case ScriptKind::Reorder:
            TEST_ASSERT_TRUE_MESSAGE(r.reorders > 0, (where + "reorder did not fire").c_str());
            break;
        }
    }
}

void test_property_marker_never_ahead_of_companion_hw(void) {
    // P04 asserted inside every matrix run above; also true on the ack path
    // with a live transfer: after each pump the marker is the companion's
    // own declared cumulative ack, never more.
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 12);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    for (int i = 0; i < 12; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
        TEST_ASSERT_TRUE(store.lastSyncedSeq() <= harness.highWater());
    }
    TEST_ASSERT_EQUAL_UINT32(12, store.lastSyncedSeq());
}
