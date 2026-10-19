// F14 queue overflow (plan §9.2 / spec §9): the oldest waiting entry is
// dropped with an explicit Unlabeled marker — plus direct unit tests of the
// PendingEventQueue ring the overflow rule is built on.

#include <unity.h>

#include <bw/confirm/pending_queue.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;
using bw::confirm::PendingEntry;
using bw::confirm::PendingEventQueue;

namespace {

// 5 eligible events before any ack: 4 fit, the oldest is dropped (spec §9).
void enqueueFiveWithoutAck(ConfirmFixture& f) {
    for (uint32_t seq = 100; seq < 105; ++seq) {
        f.mgr.onEvent(makeEvent(seq, bw::EventType::DrowsinessEvent, 1000 + seq));
    }
    f.mgr.step(2000);
}

}  // namespace

void test_overflow_drops_oldest_with_unlabeled_marker(void) {
    ConfirmFixture f;
    enqueueFiveWithoutAck(f);

    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().dropped_unlabeled);
    TEST_ASSERT_EQUAL_UINT(4, f.mgr.waitingCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);

    // Exactly one marker: UserMarked + Unlabeled + F14 detector id + dropped seq.
    TEST_ASSERT_EQUAL_UINT(1, f.sink.count());
    const bw::Event* note = f.sink.firstOfType(bw::EventType::UserMarked);
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_UINT8(bw::confirm::kDetectorId, note->detector_id);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Unlabeled),
                          static_cast<int>(note->label));
    TEST_ASSERT_EQUAL_UINT32(100, payloadSeq(*note));  // seq 100 was the oldest
    // A drop is never a label write: Unlabeled is already the stored state.
    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());
}

void test_overflow_drains_in_order_after_ack(void) {
    ConfirmFixture f;
    enqueueFiveWithoutAck(f);

    f.mgr.onAlertStatus(makeAlert(true, 3000));
    f.mgr.step(8000);  // defer passed → offer the oldest survivor
    TEST_ASSERT_EQUAL_UINT32(101, f.state().seq);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(101, 8500)));
    f.mgr.step(8600);
    TEST_ASSERT_EQUAL_UINT32(102, f.state().seq);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(102, 8700)));
    f.mgr.step(8800);
    TEST_ASSERT_EQUAL_UINT32(103, f.state().seq);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(103, 8900)));
    f.mgr.step(9000);
    TEST_ASSERT_EQUAL_UINT32(104, f.state().seq);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(104, 9100)));
    f.mgr.step(9200);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));

    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(2, stats.confirmed);
    TEST_ASSERT_EQUAL_UINT32(2, stats.rejected);
    TEST_ASSERT_EQUAL_UINT32(1, stats.dropped_unlabeled);  // only seq 100
    TEST_ASSERT_EQUAL_UINT32(0, stats.unlabeled);
    TEST_ASSERT_EQUAL_UINT(4, f.store.callCount());  // no write for the drop
    TEST_ASSERT_NULL(f.store.find(100));
}

// The ACTIVE prompt is held outside the queue while Offered, so overflow can
// never yank the event the user is being asked about (plan §11/§9.2).
void test_overflow_keeps_active_prompt(void) {
    ConfirmFixture f;
    for (uint32_t seq = 100; seq < 104; ++seq) {
        f.mgr.onEvent(makeEvent(seq, bw::EventType::DrowsinessEvent, 1000 + seq));
    }
    f.mgr.onAlertStatus(makeAlert(true, 2000));
    f.mgr.step(7000);  // offer 100; waiting = 101,102,103 (3 of 4)
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);
    TEST_ASSERT_EQUAL_UINT8(3, f.state().queue_depth);

    f.mgr.onEvent(makeEvent(104, bw::EventType::DrowsinessEvent, 1500));
    f.mgr.step(7100);
    TEST_ASSERT_EQUAL_UINT32(0, f.mgr.labelStats().dropped_unlabeled);  // 4th fits
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);

    f.mgr.onEvent(makeEvent(105, bw::EventType::DrowsinessEvent, 1600));
    f.mgr.step(7200);
    // Oldest WAITING (101) dropped with a marker; the offered prompt survives.
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().dropped_unlabeled);
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);
    const bw::Event* note = f.sink.firstOfType(bw::EventType::UserMarked);
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_UINT32(101, payloadSeq(*note));
}

// ---- direct PendingEventQueue unit tests ----------------------------------

// P3/F-2: the pushFront-eviction branch — prompt offered AND waiting queue at
// kMaxPending AND the alert re-raises. The withdrawal must put the offered
// entry back at the FRONT, evict the oldest WAITING entry with an Unlabeled
// marker, and never lose the active prompt.
// H10: without eviction handling the marker/drop counters never move and the
// queue order assertions fail.
void test_withdraw_pushfront_evicts_oldest_waiting(void) {
    ConfirmFixture f;
    for (uint32_t seq = 100; seq < 104; ++seq) {  // queue full (kMaxPending)
        f.mgr.onEvent(makeEvent(seq, bw::EventType::DrowsinessEvent, 1000 + seq));
    }
    f.mgr.onAlertStatus(makeAlert(true, 2000));
    f.mgr.step(7000);  // offer 100 → waiting 101,102,103 (3 of 4)
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);

    f.mgr.onEvent(makeEvent(104, bw::EventType::DrowsinessEvent, 2500));
    f.mgr.step(7100);  // waiting back to 4 = full; nothing dropped yet
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);
    TEST_ASSERT_EQUAL_UINT32(0, f.mgr.labelStats().dropped_unlabeled);

    f.mgr.onAlertStatus(makeAlert(false, 8000));  // alert re-raised → withdraw
    f.mgr.step(8100);
    const bw::confirm::ConfirmLabelStats after_drop = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, after_drop.dropped_unlabeled);  // 101 evicted
    TEST_ASSERT_EQUAL_UINT32(0, after_drop.unlabeled);          // nothing timed out
    TEST_ASSERT_FALSE(f.mgr.hasOfferedPrompt());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);  // 100 back in, 101 gone

    const bw_confirm_test::LabelCall* call = f.store.find(101);  // no label write…
    TEST_ASSERT_NULL(call);
    const bw::Event* note = f.sink.firstOfType(bw::EventType::UserMarked);
    TEST_ASSERT_NOT_NULL(note);  // …but the marker says which seq was dropped
    TEST_ASSERT_EQUAL_UINT32(101, bw_confirm_test::payloadSeq(*note));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Unlabeled),
                          static_cast<int>(note->label));

    f.mgr.onAlertStatus(makeAlert(true, 9000));  // acked again → defer until 14000
    f.mgr.step(14000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);  // the withdrawn prompt keeps its place
    TEST_ASSERT_EQUAL_UINT8(3, f.state().queue_depth);
}

void test_queue_push_pop_preserves_fifo_order(void) {
    PendingEventQueue q;
    PendingEntry evicted{};
    PendingEntry out{};

    for (uint32_t seq = 1; seq <= 4; ++seq) {
        TEST_ASSERT_FALSE(
            q.push(PendingEntry{10 * seq, seq, bw::EventType::DrowsinessEvent}, evicted));
    }
    TEST_ASSERT_TRUE(q.full());
    TEST_ASSERT_EQUAL_UINT(4, q.size());

    TEST_ASSERT_TRUE(q.push(PendingEntry{500, 5, bw::EventType::DrowsinessEvent}, evicted));
    TEST_ASSERT_EQUAL_UINT32(1, evicted.seq);  // oldest dropped, not the newest
    TEST_ASSERT_EQUAL_UINT(4, q.size());

    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(2, out.seq);
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(3, out.seq);

    // pushFront (prompt withdrawal) puts the entry back at the head.
    TEST_ASSERT_FALSE(q.pushFront(PendingEntry{500, 1, bw::EventType::DrowsinessEvent}, evicted));
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(1, out.seq);
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(4, out.seq);
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(5, out.seq);  // the entry that triggered the overflow
    TEST_ASSERT_FALSE(q.pop(out));
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_NULL(q.front());
}

void test_queue_remove_by_seq_keeps_ring_order(void) {
    PendingEventQueue q;
    PendingEntry evicted{};
    PendingEntry out{};

    (void)q.push(PendingEntry{10, 1, bw::EventType::DrowsinessEvent}, evicted);
    (void)q.push(PendingEntry{20, 2, bw::EventType::DrowsinessEvent}, evicted);
    (void)q.push(PendingEntry{30, 3, bw::EventType::DrowsinessEvent}, evicted);

    TEST_ASSERT_TRUE(q.removeBySeq(2, out));
    TEST_ASSERT_EQUAL_UINT32(2, out.seq);
    TEST_ASSERT_EQUAL_UINT(2, q.size());
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(1, out.seq);
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT32(3, out.seq);  // newer entry still after the removed one

    TEST_ASSERT_FALSE(q.removeBySeq(99, out));  // absent seq is a no-op
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_NULL(q.front());
}
