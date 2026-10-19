// F14 CR-016 provider output (plan §9.5): ConfirmPromptState for F12 —
// phases Idle/Deferred/Offered plus seq, offer deadline and queue depth.

#include <unity.h>

#include <bw/confirm/confirm_config.h>

#include "confirm_fixtures.h"

using bw::ConfirmPromptPhase;
using namespace bw_confirm_test;

void test_provider_false_before_first_step(void) {
    ConfirmFixture f;
    bw::ConfirmPromptState s{};
    s.phase = ConfirmPromptPhase::Offered;  // must not survive a failed read
    TEST_ASSERT_FALSE(f.mgr.latest(s));
}

void test_provider_idle_after_first_step(void) {
    ConfirmFixture f;
    f.mgr.step(0);
    bw::ConfirmPromptState s{};
    TEST_ASSERT_TRUE(f.mgr.latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(0, s.seq);
    TEST_ASSERT_EQUAL_UINT64(0, s.offer_deadline_ms);
    TEST_ASSERT_EQUAL_UINT8(0, s.queue_depth);
    TEST_ASSERT_EQUAL_UINT64(0, s.t_ms);
}

void test_provider_deferred_then_offered_fields(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onEvent(makeEvent(11, bw::EventType::DrowsinessEvent, 200));
    f.mgr.step(300);

    bw::ConfirmPromptState s{};
    TEST_ASSERT_TRUE(f.mgr.latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Deferred),
                          static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(0, s.seq);  // nothing offered yet
    TEST_ASSERT_EQUAL_UINT64(0, s.offer_deadline_ms);
    TEST_ASSERT_EQUAL_UINT8(2, s.queue_depth);
    TEST_ASSERT_EQUAL_UINT64(300, s.t_ms);

    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);
    TEST_ASSERT_TRUE(f.mgr.latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Offered), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(10, s.seq);
    TEST_ASSERT_EQUAL_UINT64(6000 + bw::confirm::kPromptTimeoutMs, s.offer_deadline_ms);
    TEST_ASSERT_EQUAL_UINT8(1, s.queue_depth);  // 11 waits BEHIND the offered 10
    TEST_ASSERT_EQUAL_UINT64(6000, s.t_ms);
}

void test_provider_idle_after_last_resolve(void) {
    ConfirmFixture f;
    f.mgr.onEvent(makeEvent(10, bw::EventType::DrowsinessEvent, 100));
    f.mgr.onAlertStatus(makeAlert(true, 1000));
    f.mgr.step(6000);
    TEST_ASSERT_TRUE(f.mgr.onCommand(makeConfirm(10, 6500)));
    f.mgr.step(6600);

    bw::ConfirmPromptState s{};
    TEST_ASSERT_TRUE(f.mgr.latest(s));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfirmPromptPhase::Idle), static_cast<int>(s.phase));
    TEST_ASSERT_EQUAL_UINT32(0, s.seq);
    TEST_ASSERT_EQUAL_UINT64(0, s.offer_deadline_ms);
    TEST_ASSERT_EQUAL_UINT8(0, s.queue_depth);
}

void test_provider_schema_version(void) {
    TEST_ASSERT_EQUAL_UINT8(1, bw::ConfirmPromptState::kSchemaVersion);
}
