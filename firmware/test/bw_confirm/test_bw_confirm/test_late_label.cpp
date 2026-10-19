// F14 late labeling (plan §9.4 / spec D1): a label may arrive for an event
// that already timed out or was dropped — the store write still happens.

#include <unity.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

void test_late_confirm_after_timeout_still_labels(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);   // offered
    f.mgr.step(36000);  // timed out → Unlabeled, queue empty
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().unlabeled);

    // The user (or F21 via the companion, D1) labels it afterwards.
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 40000)));
    const LabelCall* call = f.store.find(10);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(call->label));

    f.mgr.step(40100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.confirmed);
    TEST_ASSERT_EQUAL_UINT32(1, stats.unlabeled);  // both facts stay visible
}

void test_late_reject_unknown_seq_labels_store(void) {
    ConfirmFixture f;
    // A seq F14 has never seen (dropped by an earlier boot's queue, or a
    // companion-initiated label): handled exactly the same — best effort.
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(999, 5000)));
    const LabelCall* call = f.store.find(999);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Rejected), static_cast<int>(call->label));
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().rejected);
}

void test_confirm_for_waiting_entry_resolves_and_removes(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(100, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onEvent(makeEvent(101, bw::EventType::DrowsinessEvent, 200));
    f.mgr.step(300);
    TEST_ASSERT_EQUAL_UINT(2, f.mgr.waitingCount());

    // Explicit label for a queued-but-not-offered entry: honor and remove it.
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(101, 400)));
    TEST_ASSERT_EQUAL_UINT(1, f.mgr.waitingCount());
    TEST_ASSERT_NOT_NULL(f.store.find(101));

    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);
    TEST_ASSERT_EQUAL_UINT32(100, f.state().seq);  // 101 is gone, 100 offered
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().confirmed);
}

void test_non_label_commands_not_consumed(void) {
    ConfirmFixture f;
    bw::Command other{};
    other.type = bw::CommandType::NextScreen;
    other.t_ms = 100;
    TEST_ASSERT_FALSE(f.mgr.onCommand(other));

    other.type = bw::CommandType::MarkEvent;
    TEST_ASSERT_FALSE(f.mgr.onCommand(other));

    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());
    TEST_ASSERT_EQUAL_UINT(0, f.sink.count());
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.confirmed + stats.rejected);
}

// P3/F-1: F11's button table emits ConfirmEvent/RejectEvent with arg0 = 0
// (test/bw_input/test_input_router.cpp:88/98). arg0 == 0 means "the offered
// prompt" (seq 0 is never a valid Event::seq) — the button must label the
// event the user is actually looking at, never seq 0.
// H10: reverting the arg0==0 resolution labels seq 0 instead → find(10) is
// null and find(0) is set, so this test fails without the fix.
void test_button_confirm_with_arg0_zero_targets_offered(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);  // prompt offered for seq 10

    bw::Command button_press = makeConfirm(0, 6500);  // F11 shape: arg0=0, source=Button
    button_press.source = 0;
    TEST_ASSERT_TRUE(f.mgr.onCommand(button_press));

    const LabelCall* offered = f.store.find(10);
    TEST_ASSERT_NOT_NULL(offered);  // the OFFERED prompt got the label
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(offered->label));
    TEST_ASSERT_NULL(f.store.find(0));  // seq 0 is never written
    TEST_ASSERT_EQUAL_UINT(1, f.store.callCount());

    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.confirmed);
    f.mgr.step(6600);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
}

// N-4: the RejectEvent half of the same convention (F11 :98 emits RejectEvent
// with arg0 = 0 too) — the offered prompt gets Rejected, seq 0 is never written.
// H10: without the arg0==0 resolution this fails (labels seq 0, not seq 10).
void test_button_reject_with_arg0_zero_targets_offered(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);  // prompt offered for seq 10

    bw::Command button_press = makeReject(0, 6500);  // F11 shape: arg0=0, source=Button
    button_press.source = 0;
    TEST_ASSERT_TRUE(f.mgr.onCommand(button_press));

    const LabelCall* offered = f.store.find(10);
    TEST_ASSERT_NOT_NULL(offered);  // the OFFERED prompt got the label
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Rejected),
                          static_cast<int>(offered->label));
    TEST_ASSERT_NULL(f.store.find(0));  // seq 0 is never written
    TEST_ASSERT_EQUAL_UINT(1, f.store.callCount());

    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.rejected);
    f.mgr.step(6600);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
}

// Button press with NO prompt offered: nothing to label — the command is not
// consumed and no phantom seq-0 record or notification is produced.
void test_button_confirm_with_no_offered_prompt_is_noop(void) {
    ConfirmFixture f;
    TEST_ASSERT_FALSE(f.mgr.onCommand(makeConfirm(0, 100)));
    TEST_ASSERT_FALSE(f.mgr.onCommand(makeReject(0, 200)));

    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());
    TEST_ASSERT_EQUAL_UINT(0, f.sink.count());
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.confirmed + stats.rejected);
}
