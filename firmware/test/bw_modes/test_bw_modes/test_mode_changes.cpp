// bw_modes — transition-observation suite (F24 spec §9 cases 17–19).
//
// INTEGRATION DECISION 1 (2026-10-18): W6 emits NO events — CR-028
// (EventType::ModeChanged) was rejected/burned; detector_id 24 stays
// reserved. The planned test_mode_events.cpp therefore becomes this file:
// the same three guarantees are pinned at the ModeOutput level (the seam a
// future event-emitting P4 wiring would read), in changed-flag terms:
//   17 → exactly one `changed` step per transition, correct provenance;
//   18 → no `changed` on no-op / rejected command / power-only change;
//   19 → a rejected command leaves no stuck state and is never retried
//        (the sink-Full case itself is moot without a sink; queue-full
//        refusal lives in test_wire_modes_compile.cpp).

#include <bw/modes/mode_manager.h>
#include <unity.h>

#include "mode_test_helpers.h"

using bw_modes_test::batteryAt;
using bw_modes_test::commandAt;
using bw_modes_test::makeCommand;
using bw_modes_test::settleAt;
using bw_modes_test::settleWithBattery;
using bw_modes_test::unixAt;

// Spec §9 case 17 — exactly one `changed` step per transition; every
// transition carries its reason; settle ticks carry none.
void test_one_changed_flag_per_transition_with_reason(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};

    // Command: Day → Night.
    bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Night))));
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day),
                          static_cast<int>(r.value.previous_mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night), static_cast<int>(mgr.mode()));

    // Settle tick: no transition.
    r = mgr.step(settleAt(0));
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r.value.previous_mode));

    // AutoSchedule: the 07:00 crossing (wall time became known meanwhile —
    // legitimate, e.g. the phone synced after boot).
    (void)mgr.step(settleAt(unixAt(6, 59, 59)));
    r = mgr.step(settleAt(unixAt(7, 0, 0)));
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::AutoSchedule),
                          static_cast<int>(r.value.reason));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));

    // NapStart.
    r = mgr.step(commandAt(makeCommand(bw::CommandType::StartNap)));
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::NapStart),
                          static_cast<int>(r.value.reason));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap), static_cast<int>(mgr.mode()));

    // ResearchEnter.
    r = mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::ResearchEnter),
                          static_cast<int>(r.value.reason));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                          static_cast<int>(mgr.mode()));

    // ResearchExit back to the stored mode (Nap).
    r = mgr.step(commandAt(makeCommand(bw::CommandType::ExitResearch)));
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::ResearchExit),
                          static_cast<int>(r.value.reason));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap), static_cast<int>(mgr.mode()));
}

// Spec §9 case 18 — no `changed` (and therefore no would-be event) on a
// no-op, a rejected command, or a power-only change.
void test_no_changed_on_noop_reject_or_power_only(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};

    // No-op self-transition.
    bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_FALSE(r.value.changed);

    // Power-only change: enter LowPower.
    r = mgr.step(settleWithBattery(0, batteryAt(10.0f)));
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));

    // Rejected command (illegal SetMode from Research).
    (void)mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
    r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
}

// Spec §9 case 19 (W6 shape) — a rejected command is reported to its caller,
// leaves state unchanged, and is never retried: the next legal command works
// immediately. No retry loop can exist because step() is idempotent for the
// same input and the pump drains each queued command exactly once.
void test_rejected_command_not_retried_and_state_not_stuck(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    (void)mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));

    // Rejected SetMode: Research must be untouched.
    bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                          static_cast<int>(mgr.mode()));

    // Same input again: same rejection, same state (idempotent — the manager
    // holds no half-applied transition to "retry").
    r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                          static_cast<int>(mgr.mode()));

    // The legal way out still works at once.
    r = mgr.step(commandAt(makeCommand(bw::CommandType::ExitResearch)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));
}
