// F14 eligibility policy (plan §9.7 / spec D6): the prompt set is
// CONFIGURATION — F14 never decides what an event is, it filters on the
// configured types and ignores its own notifications (D5 loop guard).

#include <unity.h>

#include <bw/confirm/confirm_config.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

void test_ineligible_types_never_queued(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(1, bw::EventType::Boot, 10));
    f.mgr.onEvent(makeEvent(2, bw::EventType::AlertRaised, 20));
    f.mgr.onEvent(makeEvent(3, bw::EventType::UserMarked, 30));
    f.mgr.onEvent(makeEvent(4, bw::EventType::TimeSync, 40));
    f.mgr.onEvent(makeEvent(5, bw::EventType::SleepEpisode, 50));
    f.mgr.step(100);

    TEST_ASSERT_EQUAL_UINT(0, f.mgr.waitingCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(0, f.state().queue_depth);
    TEST_ASSERT_EQUAL_UINT(0, f.sink.count());
    TEST_ASSERT_EQUAL_UINT(0, f.store.setLabelCalls());
    const bw::confirm::ConfirmLabelStats stats = f.mgr.labelStats();
    TEST_ASSERT_EQUAL_UINT32(0, stats.confirmed + stats.rejected + stats.unlabeled +
                                    stats.dropped_unlabeled);
}

// D5 loop guard: our own UserMarked notifications carry detector_id 14 and
// must never re-enter the queue, even though their type is UserMarked.
void test_own_detector_events_ignored(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(1, bw::EventType::UserMarked, 10, bw::confirm::kDetectorId));
    // Even a mis-typed emission of ours (same id, eligible type) is dropped.
    f.mgr.onEvent(makeEvent(2, bw::EventType::DrowsinessEvent, 20, bw::confirm::kDetectorId));
    f.mgr.step(100);

    TEST_ASSERT_EQUAL_UINT(0, f.mgr.waitingCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT(0, f.sink.count());
}

// Wiring may widen the set: only NapPlanned prompts, the default is replaced.
void test_custom_eligibility_config(void) {
    bw::confirm::ConfirmConfig cfg;
    cfg.eligible_count = 1;
    cfg.eligible_types[0] = bw::EventType::NapPlanned;

    ConfirmFixture f(cfg);
    f.mgr.onEvent(makeEvent(1, bw::EventType::DrowsinessEvent, 10));  // default-eligible
    f.mgr.onEvent(makeEvent(2, bw::EventType::NapPlanned, 20));       // custom-eligible
    f.mgr.step(100);

    TEST_ASSERT_EQUAL_UINT(1, f.mgr.waitingCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(f.state().phase));
    TEST_ASSERT_EQUAL_UINT8(1, f.state().queue_depth);
    TEST_ASSERT_TRUE(cfg.isEligible(bw::EventType::NapPlanned));
    TEST_ASSERT_FALSE(cfg.isEligible(bw::EventType::DrowsinessEvent));
}

// The default set matches the spec §8 table (kPromptEligibleDefault).
void test_default_eligibility_matches_spec(void) {
    const bw::confirm::ConfirmConfig cfg;
    TEST_ASSERT_TRUE(cfg.isEligible(bw::EventType::DrowsinessEvent));
    TEST_ASSERT_TRUE(cfg.isEligible(bw::EventType::SuspectedCataplexy));
    TEST_ASSERT_FALSE(cfg.isEligible(bw::EventType::Boot));
    TEST_ASSERT_FALSE(cfg.isEligible(bw::EventType::AlertRaised));
    TEST_ASSERT_FALSE(cfg.isEligible(bw::EventType::UserMarked));
    TEST_ASSERT_EQUAL_UINT8(2, cfg.eligible_count);          // kPromptEligibleDefault.size()
    TEST_ASSERT_EQUAL_UINT64(10000, cfg.prompt_timeout_ms);  // 10 s demo retune 2026-10-18
    TEST_ASSERT_EQUAL_UINT64(5000, cfg.defer_after_ack_ms);
}
