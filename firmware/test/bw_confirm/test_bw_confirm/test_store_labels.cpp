// F14 store behavior (plan §9.3 / spec §9 + §10): labels persist through the
// fake IEventStore, failures are retried a bounded number of times, and a
// failing store never blocks the next prompt.

#include <unity.h>

#include <bw/confirm/confirm_config.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

namespace {

// Walks the fixture to "prompt offered for seq 10": one event, ack at 1000,
// defer satisfied at 6000.
void offeredFor10(ConfirmFixture& f) {
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);
    TEST_ASSERT_EQUAL_UINT32(10, f.state().seq);
}

}  // namespace

void test_store_labels_captured_with_seq_and_label(void) {
    ConfirmFixture f;
    offeredFor10(f);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    f.mgr.step(6600);  // offers the next entry

    f.mgr.onEvent(makeEvent(11, bw::EventType::SuspectedCataplexy, 6550));
    f.mgr.step(6700);  // queue had room: 11 waiting behind nothing → offered now
    TEST_ASSERT_EQUAL_UINT32(11, f.state().seq);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeReject(11, 7000)));

    TEST_ASSERT_EQUAL_UINT(2, f.store.callCount());
    const LabelCall* c10 = f.store.find(10);
    const LabelCall* c11 = f.store.find(11);
    TEST_ASSERT_NOT_NULL(c10);
    TEST_ASSERT_NOT_NULL(c11);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed), static_cast<int>(c10->label));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Rejected), static_cast<int>(c11->label));
}

// One transient failure: the label is retried on the next step and lands
// (spec §10), counted as exactly one retry.
void test_store_failure_retried_then_succeeds(void) {
    ConfirmFixture f;
    offeredFor10(f);
    f.store.failNext(1);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    TEST_ASSERT_EQUAL_UINT(1, f.store.setLabelCalls());  // the failed attempt
    TEST_ASSERT_EQUAL_UINT(0, f.store.callCount());      // nothing written yet
    bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.confirmed);  // the label lives in RAM (§10)
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_giveups);

    f.mgr.step(6600);  // flush retries
    stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_giveups);
    TEST_ASSERT_EQUAL_UINT(1, f.store.callCount());
    const LabelCall* call = f.store.find(10);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Confirmed),
                          static_cast<int>(call->label));
}

// Persistent failure: exactly kRetryCount total attempts, then a counted
// give-up — bounded work, no infinite loop (spec §10, plan D7).
void test_store_persistent_failure_gives_up(void) {
    ConfirmFixture f;
    offeredFor10(f);
    f.store.failNext(999);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));  // attempt 1
    f.mgr.step(6600);                                          // attempt 2
    bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(0, stats.store_giveups);

    f.mgr.step(6700);  // attempt 3 == kRetryCount → give up
    stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(2, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_giveups);

    f.mgr.step(6800);  // no further attempts
    TEST_ASSERT_EQUAL_UINT(3, f.store.setLabelCalls());
    TEST_ASSERT_EQUAL_UINT32(2, f.mgr.labelStats().store_retries);
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().store_giveups);
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().confirmed);  // RAM count kept
}

// A retrying store must not stall the prompt ladder (spec §5: never block).
void test_store_failure_does_not_block_next_prompt(void) {
    ConfirmFixture f;
    offeredFor10(f);
    f.mgr.onEvent(makeEvent(11, bw::EventType::DrowsinessEvent, 6400));
    f.store.failNext(2);  // finalize + first flush both fail

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    f.mgr.step(6600);  // flush retries AND offer the next entry in one step
    TEST_ASSERT_EQUAL_UINT32(11, f.state().seq);  // ladder keeps moving
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT32(1, f.mgr.labelStats().store_retries);  // flush ran
}

// The label-change notification is independent of the store outcome (plan D5).
void test_label_notification_survives_store_failure(void) {
    ConfirmFixture f;
    offeredFor10(f);
    f.store.failNext(1);

    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    TEST_ASSERT_EQUAL_UINT(1, f.sink.count());
    const bw::Event* note = f.sink.firstOfType(bw::EventType::UserMarked);
    TEST_ASSERT_NOT_NULL(note);
    TEST_ASSERT_EQUAL_UINT32(10, payloadSeq(*note));
    TEST_ASSERT_EQUAL_UINT32(0, f.mgr.labelStats().publish_failures);
}

// No store attached yet (F18 slot empty): the label is dropped AND counted,
// never silently lost and never a crash (spec §10, F09 D11 precedent).
void test_null_store_and_sink_count_drops(void) {
    bw::confirm::ConfirmationManager mgr;  // no store, no sink attached
    mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    mgr.onAlertStatus(makeAlert(true, 1000));
    mgr.step(6000);
    TEST_ASSERT_TRUE(mgr.onCommand(makeConfirm(10, 6500)));

    const bw::confirm::ConfirmLabelStats stats = mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.confirmed);  // counted in RAM
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_giveups);
    TEST_ASSERT_EQUAL_UINT32(1, stats.publish_failures);
}

// P3/F-3: the retry-buffer-full give-up branch (plan §11: kMaxRetryPending =
// 4) — 5 concurrently failed labels: 4 hold a retry slot, the 5th gives up
// immediately, and total store work stays bounded (kRetryCount attempts each).
// H10: without the bounded give-up, either the 5th label vanishes silently
// (giveups == 0) or the buffer overflows its fixed 4-entry storage.
void test_retry_buffer_full_gives_up_bounded(void) {
    ConfirmFixture f;
    f.store.failNext(999);

    // 5 concurrent failures: late-label commands all fail their first attempt.
    for (uint32_t seq = 1; seq <= 5; ++seq) {
        TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(seq, 1000 + seq)));
    }

    bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(5, stats.confirmed);        // all 5 kept in RAM counts
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_giveups);    // 5th: buffer full
    TEST_ASSERT_EQUAL_UINT(5, f.store.setLabelCalls());  // one attempt each

    f.mgr.step(2000);  // flush: 4 attempts fail → attempts 2 each
    stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(4, stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(1, stats.store_giveups);

    f.mgr.step(3000);  // flush: 4 attempts fail → attempts 3 == kRetryCount
    stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(8, stats.store_retries);  // 4 + 4, nothing more
    TEST_ASSERT_EQUAL_UINT32(5, stats.store_giveups);  // all bounded give-ups

    f.mgr.step(4000);
    f.mgr.step(5000);                                     // buffer empty: no further store traffic
    TEST_ASSERT_EQUAL_UINT(13, f.store.setLabelCalls());  // 5 + 4 + 4
    const bw::confirm::ConfirmLabelStats final_stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(8, final_stats.store_retries);
    TEST_ASSERT_EQUAL_UINT32(5, final_stats.store_giveups);
}
