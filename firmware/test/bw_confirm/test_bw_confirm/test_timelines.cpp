// F14 scripted timelines (plan §9.1 / spec §9): event → alert → ack → prompt
// → confirm / reject / timeout, all driven by explicit TimeMs values.

#include <unity.h>

#include <bw/confirm/confirm_config.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

// §9a: event → deferred → ack edge → defer window → offered → confirm.
void test_timeline_event_to_confirm(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.step(100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(1, f.state().queue_depth);

    f.mgr.onAlertStatus(makeAlert(true, 1000));  // ack edge → defer until 6000
    f.mgr.step(1000);                            // inside the defer window: still Deferred (D1)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());  // nothing prompted yet

    f.mgr.step(6000);  // exactly kPromptDeferAfterAckMs after the ack
    bw::ConfirmPromptState s = f.state();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(10, s.seq);
    TEST_ASSERT_EQUAL_UINT64(6000 + bw::confirm::kPromptTimeoutMs, s.offer_deadline_ms);
    TEST_ASSERT_EQUAL_UINT8(0, s.queue_depth);  // offered entry is not "behind" itself

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    TEST_ASSERT_EQUAL_UINT(1, f.store.callCount());
    const LabelCall* call = f.store.find(10);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(call->label));

    // D5 notification: UserMarked, F14's detector id, payload carries the seq.
    TEST_ASSERT_EQUAL_UINT(1, f.sink.count());
    const bw::Event* note = f.sink.firstOfType(bw::EventType::UserMarked);
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_UINT8(bw::confirm::kDetectorId, note->detector_id);
    TEST_ASSERT_EQUAL_UINT32(10, payloadSeq(*note));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(note->label));

    f.mgr.step(6600);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.confirmed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.unlabeled);
}

// §9a: the reject path stores Rejected (spec §3 output).
void test_timeline_event_to_reject(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(20, bw::EventType::SuspectedCataplexy, 100));
    f.mgr.onAlertStatus(makeAlert(true, 500));
    f.mgr.step(5500);  // offer at 500 + 5000
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(20, 6000)));
    const LabelCall* call = f.store.find(20);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Rejected), static_cast<int>(call->label));
    f.mgr.step(6100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().rejected);
}

// §9b: timeout resolves Unlabeled with NO store write (Unlabeled is already
// the stored state) and the next queued entry is offered in the same step.
void test_timeline_prompt_timeout_unlabeled(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onEvent(makeEvent(11, bw::EventType::DrowsinessEvent, 200));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    constexpr bw::TimeMs kT = bw::confirm::kPromptTimeoutMs;
    f.mgr.step(6000);           // offer 10, deadline 6000 + kT
    f.mgr.step(6000 + kT - 1);  // one ms before the deadline: still offered
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT32(10, f.state().seq);

    f.mgr.step(6000 + kT);
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().unlabeled);
    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());  // never penalized, never written
    bw::ConfirmPromptState s = f.state();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(11, s.seq);                           // next entry, same step
    TEST_ASSERT_EQUAL_UINT64(6000 + 2 * kT, s.offer_deadline_ms);  // offer 11 + kT

    f.mgr.step(6000 + 2 * kT);  // 11 times out too
    TEST_ASSERT_EQUAL_UINT32(2, f.mgr.labelStats().unlabeled);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
}

// §9c: a new alert (acknowledged=false) withdraws the prompt back to the
// queue; the next ack re-offers the SAME event after one defer window.
void test_timeline_new_alert_withdraws_prompt(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(30, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);  // offered
    TEST_ASSERT_EQUAL_UINT32(30, f.state().seq);

    f.mgr.onAlertStatus(makeAlert(false, 7000));  // alert re-raised
    f.mgr.step(7100);
    bw::ConfirmPromptState s = f.state();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(0, s.seq);         // withdrawn, nothing offered
    TEST_ASSERT_EQUAL_UINT8(1, s.queue_depth);  // kept at the FRONT of the queue
    TEST_ASSERT_EQUAL_UINT32(0, f.mgr.labelStats().unlabeled);

    f.mgr.onAlertStatus(makeAlert(true, 8000));  // acked again → defer until 13000
    f.mgr.step(13000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT32(30, f.state().seq);  // same event re-offered
}

// Never-ack timeline: prompts are never offered, the queue stays bounded and
// nothing is written — no answer must never block or penalize (spec §1, D1).
void test_timeline_never_acks_stays_deferred(void) {
    ConfirmFixture f;
    for (uint32_t seq = 100; seq < 104; ++seq) {
        f.mgr.onEvent(makeEvent(seq, bw::EventType::DrowsinessEvent, seq * 10));
    }
    f.mgr.onAlertStatus(makeAlert(false, 5000));
    f.mgr.step(9000);
    f.mgr.onAlertStatus(makeAlert(false, 15000));
    f.mgr.step(20000);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(4, f.state().queue_depth);  // kMaxPending, bounded
    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());
    TEST_ASSERT_EQUAL_UINT(0, f.sink.count());  // no drops at exactly kMaxPending
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.unlabeled + stats.dropped_unlabeled + stats.confirmed +
                                    stats.rejected);
}

// Exact defer boundary: offered AT t_ack + kPromptDeferAfterAckMs, not before.
void test_timeline_offer_at_exact_defer_boundary(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(40, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(5999);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    f.mgr.step(6000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
}
