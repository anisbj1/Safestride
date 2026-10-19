// bw_sync — SyncSession flow / ack / window / timeouts (F20 §9 T01–T14).
// Deterministic: time is passed explicitly to step(), the transport is a
// scripted lossy fake, the companion is the PhoneHarness (D2).

#include <unity.h>

#include <cstdint>

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

// Store wrapper that HIDES one seq from readFrom — models an evicted/corrupt
// record, the only way the dense InMemoryEventStore can have an interior gap.
class GapStore final : public bw::IEventStore {
public:
    GapStore(bw::IEventStore& inner, uint32_t gap) : inner_(inner), gap_(gap) {}

    bw::Result<uint32_t> append(const bw::Event& e) override { return inner_.append(e); }
    bw::Result<size_t> readFrom(uint32_t from_seq, bw::Event* out, size_t max) override {
        bw::Event scratch[16];
        bw::Result<size_t> r = inner_.readFrom(from_seq, scratch, max <= 16 ? max : 16);
        if (!r.ok()) {
            return r;
        }
        size_t written = 0;
        for (size_t i = 0; i < r.value; ++i) {
            if (scratch[i].seq == gap_) {
                continue;  // "record gone": the exists-check must reject it
            }
            out[written++] = scratch[i];
        }
        return {bw::Err::Ok, written};
    }
    bw::Result<void> setLabel(uint32_t seq, bw::UserLabel l) override {
        return inner_.setLabel(seq, l);
    }
    bw::Result<void> markSyncedThrough(uint32_t seq) override {
        return inner_.markSyncedThrough(seq);
    }
    uint32_t lastSyncedSeq() const override { return inner_.lastSyncedSeq(); }

private:
    bw::IEventStore& inner_;
    uint32_t gap_;
};

// -- T01 happy path ------------------------------------------------------------

void test_session_happy_path_syncs_and_marks(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 8);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(8, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(8, harness.highWater());
    TEST_ASSERT_EQUAL_UINT32(8, store.lastSyncedSeq());  // markSyncedThrough(head)
    TEST_ASSERT_EQUAL_UINT(0, harness.corruptChunks());
    TEST_ASSERT_EQUAL_UINT(0, session.acksIgnored());
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::Idle);
    bw::SyncStatus status{};
    TEST_ASSERT_TRUE(session.latest(status));
    TEST_ASSERT_EQUAL_UINT32(8, status.last_seq);
    TEST_ASSERT_EQUAL_UINT32(8, status.last_synced_seq);
    TEST_ASSERT_EQUAL_UINT16(k::kSyncProtocolVersion, status.protocol_version);
}

// -- T02 empty store ------------------------------------------------------------

void test_session_empty_store_status_only(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());  // status frame only
    TEST_ASSERT_EQUAL_UINT(0, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
    TEST_ASSERT_TRUE(harness.statusFrames() >= 1);
}

// -- T03 request from mid-history ------------------------------------------------

void test_session_request_from_mid_history(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 10);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(7));  // phone hw = 6
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(4, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT(1, harness.storedSeqs().count(7));
    TEST_ASSERT_EQUAL_UINT(1, harness.storedSeqs().count(10));
    TEST_ASSERT_EQUAL_UINT(0, harness.storedSeqs().count(6));
    TEST_ASSERT_EQUAL_UINT32(10, store.lastSyncedSeq());
}

// -- T04 max cap (bounded + unbounded) -------------------------------------------

void test_session_max_cap_honored_exactly(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 10);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1, 3));  // cap = 3 events this round
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(3, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(3, store.lastSyncedSeq());

    TEST_ASSERT_TRUE(harness.requestFrom(4));  // resume; max = 0 = unbounded
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(10, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(10, store.lastSyncedSeq());
}

void test_session_max_zero_streams_everything(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 12);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1, 0));  // 0 = no request cap (§3.1)
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(12, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(12, store.lastSyncedSeq());
}

// -- T05 status query -------------------------------------------------------------

void test_session_status_query_gets_status(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 2);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);  // link-up STATUS
    ++now;
    (void)harness.drain();
    const size_t before = harness.statusFrames();

    TEST_ASSERT_TRUE(harness.statusQuery());  // all-zero CURSOR (§3.1)
    (void)session.step(now);
    ++now;
    (void)harness.drain();
    TEST_ASSERT_EQUAL_UINT(before + 1, harness.statusFrames());
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());  // query streams nothing
}

// -- T06 status indications on link up and complete --------------------------------

void test_session_status_flags_at_edges(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 2);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    (void)session.step(now);  // link-up edge
    ++now;
    (void)harness.drain();
    TEST_ASSERT_TRUE(harness.statusFrames() >= 1);
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, harness.lastStatusFlags());
    TEST_ASSERT_BITS_LOW(k::kStatusFlagStreaming, harness.lastStatusFlags());

    // Mid-stream the provider shows the streaming flag (§3.2 bit1)…
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    // …and the SAME flags must be present in the STATUS frame the phone
    // actually parses: the all-zero status query (§3.1) forces a STATUS
    // indication inside this very step, while the engine is still Streaming.
    // (Audit F-2a: asserting only via the provider seam would let a
    // receiver-side flag regression pass unnoticed.)
    TEST_ASSERT_TRUE(harness.statusQuery());
    (void)session.step(now);
    ++now;
    bw::SyncStatus mid{};
    TEST_ASSERT_TRUE(session.latest(mid));
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, mid.flags);
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagStreaming, mid.flags);
    (void)harness.drain();
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, harness.lastStatusFlags());
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagStreaming, harness.lastStatusFlags());
    TEST_ASSERT_BITS_LOW(k::kStatusFlagTransferAborted, harness.lastStatusFlags());
    // Frame and provider are built by the one statusFlags() helper in the
    // same step: byte-for-byte agreement, so the two seams cannot drift (F-2b).
    TEST_ASSERT_EQUAL_UINT8(mid.flags, harness.lastStatusFlags());

    f20test::runToQuiescence(session, fake, harness, now);
    // On completion the last STATUS carries linkUp, no streaming, no abort.
    TEST_ASSERT_BITS_HIGH(k::kStatusFlagLinkUp, harness.lastStatusFlags());
    TEST_ASSERT_BITS_LOW(k::kStatusFlagStreaming, harness.lastStatusFlags());
    TEST_ASSERT_BITS_LOW(k::kStatusFlagTransferAborted, harness.lastStatusFlags());
    TEST_ASSERT_EQUAL_UINT32(2, harness.lastStatusLastSynced());
}

// -- T07 flow control ---------------------------------------------------------------

void test_session_window_never_exceeds_max_unacked(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 30);

    fake.setLinkUp(true);
    harness.setSuppressAcks(true);  // nobody answers: the window must cap itself
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    for (int i = 0; i < 6; ++i) {
        (void)session.step(now);
        ++now;
        (void)harness.drain();
        TEST_ASSERT_TRUE(session.inFlightChunks() <= k::kMaxUnackedChunks);
    }
    // Window full => the engine stopped sending (4 chunks of 5+5+5+1 = 16 events).
    TEST_ASSERT_EQUAL_UINT(k::kMaxUnackedChunks, session.inFlightChunks());
    TEST_ASSERT_EQUAL_UINT(4, fake.dataFramesAccepted());
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::WaitAck);

    // Ack slides the window; the rest streams.
    harness.setSuppressAcks(false);
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT(30, harness.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(30, store.lastSyncedSeq());
}

// -- T08/T09 marker exactly once, duplicates -----------------------------------------

void test_session_ack_advances_marker_exactly_once(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 3);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(3, store.lastSyncedSeq());

    // No further frames: pumping must not move the marker again.
    for (int i = 0; i < 5; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
    }
    TEST_ASSERT_EQUAL_UINT32(3, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(0, session.acksIgnored());
}

void test_session_duplicate_ack_is_ignored(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 3);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(3, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(0, session.acksIgnored());

    // Re-send the same cumulative ack: one ignored count, marker untouched.
    bw::sync::CursorRequest dup;
    dup.ack_seq = 3;
    uint8_t frame[k::kCursorFrameBytes];
    bw::sync::encodeCursorFrame(dup, frame);
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, frame, sizeof frame));
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(3, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(1, session.acksIgnored());
}

// -- T10/T11/T12 bad acks ----------------------------------------------------------

void test_session_backward_ack_ignored(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 6);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    f20test::runToQuiescence(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());

    bw::sync::CursorRequest back;
    back.ack_seq = 3;  // < lastSyncedSeq
    uint8_t frame[k::kCursorFrameBytes];
    bw::sync::encodeCursorFrame(back, frame);
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, frame, sizeof frame));
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(1, session.acksIgnored());
}

void test_session_ack_of_missing_seq_ignored(void) {
    bw::testkit::InMemoryEventStore<> inner;
    GapStore store(inner, 3);  // seq 3's record is "gone" (evicted/corrupt)
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 6);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    bw::sync::CursorRequest missing;
    missing.ack_seq = 3;  // not in the store: exists-check must reject
    uint8_t frame[k::kCursorFrameBytes];
    bw::sync::encodeCursorFrame(missing, frame);
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, frame, sizeof frame));
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(1, session.acksIgnored());

    bw::sync::CursorRequest valid;
    valid.ack_seq = 6;  // present: applied
    bw::sync::encodeCursorFrame(valid, frame);
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, frame, sizeof frame));
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(6, store.lastSyncedSeq());
}

void test_session_ack_beyond_head_ignored(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 5);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    bw::sync::CursorRequest wild;
    wild.ack_seq = 99;  // far beyond the store head (a confused phone)
    uint8_t frame[k::kCursorFrameBytes];
    bw::sync::encodeCursorFrame(wild, frame);
    TEST_ASSERT_TRUE(fake.pushToDevice(bw::SyncChannel::Control, frame, sizeof frame));
    f20test::pumpAndDrain(session, fake, harness, now);
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());  // marker never moves on garbage
    TEST_ASSERT_EQUAL_UINT(1, session.acksIgnored());
}

// -- T13 new request mid-stream --------------------------------------------------------

void test_session_new_request_mid_stream_restarts(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 30);

    fake.setLinkUp(true);
    harness.setSuppressAcks(true);  // keep the first stream in flight
    bw::TimeMs now = 0;
    TEST_ASSERT_TRUE(harness.requestFrom(1));
    for (int i = 0; i < 3; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
    }
    TEST_ASSERT_TRUE(session.phase() == SyncPhase::WaitAck);

    // The phone changes its mind: restart at 20 (newest CURSOR is authoritative).
    harness.setSuppressAcks(false);
    TEST_ASSERT_TRUE(harness.requestFrom(20));
    f20test::runToQuiescence(session, fake, harness, now);

    TEST_ASSERT_EQUAL_UINT(0, harness.storedSeqs().count(17));  // phone skipped 17..19
    TEST_ASSERT_EQUAL_UINT(1, harness.storedSeqs().count(20));
    TEST_ASSERT_EQUAL_UINT(1, harness.storedSeqs().count(30));
    TEST_ASSERT_EQUAL_UINT32(30, store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT(0, session.acksIgnored());
}

// -- T14 never talk first ---------------------------------------------------------------

void test_session_without_request_never_streams(void) {
    bw::testkit::InMemoryEventStore<> store;
    LossyTransportFake fake;
    PhoneHarness harness(fake);
    SyncSession session;
    session.bind(&fake, &store);
    f20test::fillStore(store, 5);

    fake.setLinkUp(true);
    bw::TimeMs now = 0;
    for (int i = 0; i < 5; ++i) {
        f20test::pumpAndDrain(session, fake, harness, now);
    }
    TEST_ASSERT_EQUAL_UINT(0, fake.dataFramesAccepted());     // the device never speaks first
    TEST_ASSERT_EQUAL_UINT(1, fake.controlFramesAccepted());  // exactly the link-up STATUS
    TEST_ASSERT_EQUAL_UINT32(0, store.lastSyncedSeq());
}
