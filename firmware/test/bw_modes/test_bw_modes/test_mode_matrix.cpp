// bw_modes — transition-matrix suite (F24 spec §9 cases 1–6, §7.1, §6 D3).
// Every (from,to) pair mirrors the §7.1 matrix row by row, table-driven.

#include <bw/modes/mode_manager.h>
#include <unity.h>

#include "mode_test_helpers.h"

using bw_modes_test::commandAt;
using bw_modes_test::makeCommand;
using bw_modes_test::settleAt;

namespace {

// The expected effect of SetMode(to) from `from`, spelled out cell by cell
// (spec §7.1). Deliberately literal — NOT the predicate "to != from &&
// from != Research" — so any matrix change in the spec must be edited here
// too and fails loudly if it is not.
enum class Cell { Legal, NoOp, Reject };

struct MatrixRow {
    bw::OperatingMode from;
    bw::OperatingMode to;
    Cell expect;
};

constexpr MatrixRow kMatrix[] = {
    // from Day (row 1). The Research column is EnterResearch-only (§7.1):
    // SetMode(Research) is rejected from every non-Research mode (F24-A1).
    {bw::OperatingMode::Day, bw::OperatingMode::Day, Cell::NoOp},
    {bw::OperatingMode::Day, bw::OperatingMode::Night, Cell::Legal},
    {bw::OperatingMode::Day, bw::OperatingMode::Nap, Cell::Legal},
    {bw::OperatingMode::Day, bw::OperatingMode::Calibration, Cell::Legal},
    {bw::OperatingMode::Day, bw::OperatingMode::Research, Cell::Reject},
    // from Night (row 2)
    {bw::OperatingMode::Night, bw::OperatingMode::Day, Cell::Legal},
    {bw::OperatingMode::Night, bw::OperatingMode::Night, Cell::NoOp},
    {bw::OperatingMode::Night, bw::OperatingMode::Nap, Cell::Legal},
    {bw::OperatingMode::Night, bw::OperatingMode::Calibration, Cell::Legal},
    {bw::OperatingMode::Night, bw::OperatingMode::Research, Cell::Reject},
    // from Nap (row 3)
    {bw::OperatingMode::Nap, bw::OperatingMode::Day, Cell::Legal},
    {bw::OperatingMode::Nap, bw::OperatingMode::Night, Cell::Legal},
    {bw::OperatingMode::Nap, bw::OperatingMode::Nap, Cell::NoOp},
    {bw::OperatingMode::Nap, bw::OperatingMode::Calibration, Cell::Legal},
    {bw::OperatingMode::Nap, bw::OperatingMode::Research, Cell::Reject},
    // from Calibration (row 4)
    {bw::OperatingMode::Calibration, bw::OperatingMode::Day, Cell::Legal},
    {bw::OperatingMode::Calibration, bw::OperatingMode::Night, Cell::Legal},
    {bw::OperatingMode::Calibration, bw::OperatingMode::Nap, Cell::Legal},
    {bw::OperatingMode::Calibration, bw::OperatingMode::Calibration, Cell::NoOp},
    {bw::OperatingMode::Calibration, bw::OperatingMode::Research, Cell::Reject},
    // from Research (row 5): only ExitResearch may leave; SetMode always rejects
    // except the (Research, Research) self-transition, which is the no-op cell.
    {bw::OperatingMode::Research, bw::OperatingMode::Day, Cell::Reject},
    {bw::OperatingMode::Research, bw::OperatingMode::Night, Cell::Reject},
    {bw::OperatingMode::Research, bw::OperatingMode::Nap, Cell::Reject},
    {bw::OperatingMode::Research, bw::OperatingMode::Calibration, Cell::Reject},
    {bw::OperatingMode::Research, bw::OperatingMode::Research, Cell::NoOp},
};

// Drives a fresh manager to `mode` with explicit commands only (unknown wall
// time in these tests, so the D4 scheduler can never interfere). Research is
// reached via EnterResearch — SetMode(Research) is rejected (F24-A1, §7.1).
void driveTo(bw::modes::ModeManager& mgr, bw::OperatingMode mode) {
    if (mode == bw::OperatingMode::Day) {
        return;  // boot mode (D8)
    }
    const bw::CommandType how = (mode == bw::OperatingMode::Research)
                                    ? bw::CommandType::EnterResearch
                                    : bw::CommandType::SetMode;
    const bw::Result<bw::modes::ModeOutput> r =
        mgr.step(commandAt(makeCommand(how, static_cast<uint32_t>(mode))));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(mode), static_cast<int>(mgr.mode()));
}

}  // namespace

// Spec §9 case 1 — all 25 (from,to) pairs mirror §7.1.
void test_matrix_setmode_all_25_pairs(void) {
    static_assert(sizeof(kMatrix) / sizeof(kMatrix[0]) == 25, "§7.1 is 5x5");
    for (const MatrixRow& row : kMatrix) {
        bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
        driveTo(mgr, row.from);
        const bw::Result<bw::modes::ModeOutput> r = mgr.step(
            commandAt(makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(row.to))));
        switch (row.expect) {
        case Cell::Legal:
            TEST_ASSERT_TRUE(r.ok());
            TEST_ASSERT_TRUE(r.value.changed);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(row.to), static_cast<int>(mgr.mode()));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(row.from),
                                  static_cast<int>(r.value.previous_mode));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::Command),
                                  static_cast<int>(r.value.reason));
            break;
        case Cell::NoOp:
            TEST_ASSERT_TRUE(r.ok());  // D3.5: idempotent success
            TEST_ASSERT_FALSE(r.value.changed);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(row.from), static_cast<int>(mgr.mode()));
            break;
        case Cell::Reject:
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
            TEST_ASSERT_FALSE(r.value.changed);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(row.from), static_cast<int>(mgr.mode()));
            break;
        }
    }
}

// Spec §9 case 2 — self-transition: ok, unchanged, no `changed` flag.
void test_self_transition_is_noop(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    const bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day),
                          static_cast<int>(r.value.previous_mode));
}

// Spec §9 case 3 — EnterResearch stores/restores the pre-mode; repeat enter
// is rejected without clobbering the stored pre-mode.
void test_enter_research_stores_and_restores(void) {
    const bw::OperatingMode starts[] = {bw::OperatingMode::Day, bw::OperatingMode::Nap,
                                        bw::OperatingMode::Calibration};
    for (const bw::OperatingMode from : starts) {
        bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
        driveTo(mgr, from);

        bw::Result<bw::modes::ModeOutput> r =
            mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
        TEST_ASSERT_TRUE(r.ok());
        TEST_ASSERT_TRUE(r.value.changed);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                              static_cast<int>(mgr.mode()));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::ResearchEnter),
                              static_cast<int>(r.value.reason));

        // Repeat EnterResearch ⇒ error, state unchanged.
        r = mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
        TEST_ASSERT_FALSE(r.value.changed);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                              static_cast<int>(mgr.mode()));

        // Exit restores the STORED mode, not Day (the stored value survived
        // the rejected repeat above).
        r = mgr.step(commandAt(makeCommand(bw::CommandType::ExitResearch)));
        TEST_ASSERT_TRUE(r.ok());
        TEST_ASSERT_TRUE(r.value.changed);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(from), static_cast<int>(mgr.mode()));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::ResearchExit),
                              static_cast<int>(r.value.reason));
    }
}

// Spec §9 case 4 — ExitResearch outside Research ⇒ Err::InvalidArg, unchanged.
void test_exit_research_only_legal_in_research(void) {
    const bw::OperatingMode others[] = {bw::OperatingMode::Day, bw::OperatingMode::Night,
                                        bw::OperatingMode::Nap, bw::OperatingMode::Calibration};
    for (const bw::OperatingMode from : others) {
        bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
        driveTo(mgr, from);
        const bw::Result<bw::modes::ModeOutput> r =
            mgr.step(commandAt(makeCommand(bw::CommandType::ExitResearch)));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
        TEST_ASSERT_FALSE(r.value.changed);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(from), static_cast<int>(mgr.mode()));
    }
}

// Spec §9 case 5 — StartNap: Day→Nap with reason NapStart; illegal in Research.
void test_start_nap_semantics(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    bw::Result<bw::modes::ModeOutput> r =
        mgr.step(commandAt(makeCommand(bw::CommandType::StartNap)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap), static_cast<int>(mgr.mode()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::NapStart),
                          static_cast<int>(r.value.reason));

    // Repeat StartNap while already in Nap: idempotent no-op (D3.5).
    r = mgr.step(commandAt(makeCommand(bw::CommandType::StartNap)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap), static_cast<int>(mgr.mode()));

    // In Research: rejected.
    driveTo(mgr, bw::OperatingMode::Research);
    r = mgr.step(commandAt(makeCommand(bw::CommandType::StartNap)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                          static_cast<int>(mgr.mode()));
}

// Spec §9 case 6 — out-of-enum SetMode argument ⇒ Err::InvalidArg, unchanged.
void test_setmode_out_of_enum_rejected(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    const bw::Result<bw::modes::ModeOutput> r =
        mgr.step(commandAt(makeCommand(bw::CommandType::SetMode, 200)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));

    // 5 is the first invalid value (OperatingMode has kModeCount = 5 members).
    const bw::Result<bw::modes::ModeOutput> r5 =
        mgr.step(commandAt(makeCommand(bw::CommandType::SetMode, 5)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r5.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));
}

// Spec §9 case 6b (F24-A1, audit 2026-10-18) — SetMode(Research) is rejected
// from every non-Research mode (§7.1: the Research column is EnterResearch-
// only), and the pre-Research context survives: entering Research is possible
// only through the path that stores pre_research_mode_. Reverting the
// target==Research guard in applyCommand() fails both this test and the four
// Reject cells of the table above (a SetMode(Research) entry would make
// ExitResearch restore the stale Day default instead of Nap).
void test_setmode_to_research_rejected_context_preserved(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    driveTo(mgr, bw::OperatingMode::Nap);

    const bw::Result<bw::modes::ModeOutput> rejected = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Research))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(rejected.err));
    TEST_ASSERT_FALSE(rejected.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap),
                          static_cast<int>(mgr.mode()));  // refused, not applied

    // The legal entry stores the context, and ExitResearch restores it.
    bw::Result<bw::modes::ModeOutput> r =
        mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Research),
                          static_cast<int>(mgr.mode()));
    r = mgr.step(commandAt(makeCommand(bw::CommandType::ExitResearch)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Nap),
                          static_cast<int>(mgr.mode()));  // context preserved
}

// Spec §10 — a non-mode command (e.g. NextScreen, routed here by mistake) has
// exactly one legal effect in F24: none. Rejected, never silently swallowed.
void test_foreign_command_rejected(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    const bw::Result<bw::modes::ModeOutput> r =
        mgr.step(commandAt(makeCommand(bw::CommandType::NextScreen)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(r.err));
    TEST_ASSERT_FALSE(r.value.changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(mgr.mode()));
}
