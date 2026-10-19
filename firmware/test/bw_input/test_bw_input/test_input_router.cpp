// F11 §9 / DoD §14 — the routing suite is the D2-N table: one test per cell
// (5 gestures x 4 contexts = 20) plus a completeness test that proves the
// table has exactly those 20 cells. If spec and tests ever diverge, one of
// these files fails.

#include <bw/input/input_router.h>

#include <unity.h>

namespace {

using bw::input::GestureKey;
using bw::input::InputContextId;

// Asserts one D2-N cell: routing decision, exact command envelope (type, args,
// source byte, gesture timestamp).
void expectCell(InputContextId ctx, GestureKey key, bool has, bw::CommandType type, uint32_t arg0,
                uint32_t arg1) {
    bw::input::InputRouter router;
    const bw::input::GestureRecord rec{12345, key};
    bw::Command cmd{};
    const bool routed = router.route(ctx, rec, cmd);
    TEST_ASSERT_EQUAL(has ? 1 : 0, routed ? 1 : 0);
    if (!has) {
        return;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(type), static_cast<int>(cmd.type));
    TEST_ASSERT_EQUAL_UINT32(arg0, cmd.arg0);
    TEST_ASSERT_EQUAL_UINT32(arg1, cmd.arg1);
    TEST_ASSERT_EQUAL_UINT8(bw::input::kCommandSourceButton, cmd.source);
    TEST_ASSERT_EQUAL_UINT64(12345, cmd.t_ms);
}

// --- Idle ------------------------------------------------------------------
void test_route_idle_big_short(void) {
    expectCell(InputContextId::Idle, GestureKey::BigShort, true, bw::CommandType::OpenMenu, 0, 0);
}

void test_route_idle_big_long(void) {
    expectCell(InputContextId::Idle, GestureKey::BigLong, false, bw::CommandType::AcknowledgeAlert,
               0, 0);
}

void test_route_idle_small_short(void) {
    expectCell(InputContextId::Idle, GestureKey::SmallShort, true, bw::CommandType::NextScreen,
               bw::input::kStepOnce, 0);
}

void test_route_idle_small_long(void) {
    expectCell(InputContextId::Idle, GestureKey::SmallLong, true, bw::CommandType::MarkEvent, 0, 0);
}

void test_route_idle_chord(void) {
    expectCell(InputContextId::Idle, GestureKey::Chord, true, bw::CommandType::StartPairing, 0, 0);
}

// --- AlertActive -----------------------------------------------------------
void test_route_alert_big_short(void) {
    expectCell(InputContextId::AlertActive, GestureKey::BigShort, true,
               bw::CommandType::AcknowledgeAlert, 0, 0);
}

void test_route_alert_big_long(void) {
    expectCell(InputContextId::AlertActive, GestureKey::BigLong, true, bw::CommandType::CancelAlert,
               0, 0);
}

void test_route_alert_small_short(void) {
    expectCell(InputContextId::AlertActive, GestureKey::SmallShort, true,
               bw::CommandType::NextScreen, bw::input::kStepOnce, 0);
}

void test_route_alert_small_long(void) {
    expectCell(InputContextId::AlertActive, GestureKey::SmallLong, false,
               bw::CommandType::AcknowledgeAlert, 0, 0);
}

void test_route_alert_chord(void) {
    expectCell(InputContextId::AlertActive, GestureKey::Chord, true, bw::CommandType::StartPairing,
               0, 0);
}

// --- ConfirmPrompt ---------------------------------------------------------
// §13 conflict resolved: reject lives here (Small short); mark (Small long)
// exists only in Idle, so the two can never both be reachable.
void test_route_prompt_big_short(void) {
    expectCell(InputContextId::ConfirmPrompt, GestureKey::BigShort, true,
               bw::CommandType::ConfirmEvent, 0, 0);
}

void test_route_prompt_big_long(void) {
    expectCell(InputContextId::ConfirmPrompt, GestureKey::BigLong, false,
               bw::CommandType::AcknowledgeAlert, 0, 0);
}

void test_route_prompt_small_short(void) {
    expectCell(InputContextId::ConfirmPrompt, GestureKey::SmallShort, true,
               bw::CommandType::RejectEvent, 0, 0);
}

void test_route_prompt_small_long(void) {
    expectCell(InputContextId::ConfirmPrompt, GestureKey::SmallLong, false,
               bw::CommandType::AcknowledgeAlert, 0, 0);
}

void test_route_prompt_chord(void) {
    expectCell(InputContextId::ConfirmPrompt, GestureKey::Chord, true,
               bw::CommandType::StartPairing, 0, 0);
}

// --- Menu ------------------------------------------------------------------
void test_route_menu_big_short(void) {
    expectCell(InputContextId::Menu, GestureKey::BigShort, false, bw::CommandType::AcknowledgeAlert,
               0, 0);
}

void test_route_menu_big_long(void) {
    expectCell(InputContextId::Menu, GestureKey::BigLong, false, bw::CommandType::AcknowledgeAlert,
               0, 0);
}

void test_route_menu_small_short(void) {
    expectCell(InputContextId::Menu, GestureKey::SmallShort, true, bw::CommandType::NextScreen,
               bw::input::kStepOnce, 0);
}

void test_route_menu_small_long(void) {
    expectCell(InputContextId::Menu, GestureKey::SmallLong, false,
               bw::CommandType::AcknowledgeAlert, 0, 0);
}

void test_route_menu_chord(void) {
    expectCell(InputContextId::Menu, GestureKey::Chord, true, bw::CommandType::StartPairing, 0, 0);
}

// The table must contain exactly the 4 x 5 = 20 cells, each (context, gesture)
// pair once — no implicit default, no duplicates (completeness by construction).
void test_route_table_complete_no_duplicates(void) {
    TEST_ASSERT_EQUAL_UINT(20, bw::input::kInputMap.size());
    for (uint8_t c = 0; c < bw::input::kInputContextCount; ++c) {
        for (uint8_t g = 0; g < bw::input::kGestureKeyCount; ++g) {
            const auto ctx = static_cast<InputContextId>(c);
            const auto key = static_cast<GestureKey>(g);
            uint8_t hits = 0;
            for (const bw::input::MapCell& cell : bw::input::kInputMap) {
                if (cell.context == ctx && cell.gesture == key) {
                    ++hits;
                }
            }
            TEST_ASSERT_EQUAL_UINT8(1, hits);
        }
    }
}

// "no command" cells: route() returns false and leaves the out Command
// untouched (no sentinel type can ever leak into a Command).
void test_route_no_command_cells_stay_unrouted(void) {
    bw::input::InputRouter router;
    // The 7 no-command cells of D2-N, explicit: Idle/BigLong,
    // AlertActive/SmallLong, ConfirmPrompt/BigLong, ConfirmPrompt/SmallLong,
    // Menu/BigShort, Menu/BigLong, Menu/SmallLong.
    const InputContextId ctxs[7] = {InputContextId::Idle,          InputContextId::AlertActive,
                                    InputContextId::ConfirmPrompt, InputContextId::ConfirmPrompt,
                                    InputContextId::Menu,          InputContextId::Menu,
                                    InputContextId::Menu};
    const GestureKey keys[7] = {GestureKey::BigLong,   GestureKey::SmallLong, GestureKey::BigLong,
                                GestureKey::SmallLong, GestureKey::BigShort,  GestureKey::BigLong,
                                GestureKey::SmallLong};
    for (uint8_t i = 0; i < 7; ++i) {
        const bw::input::GestureRecord rec{777, keys[i]};
        bw::Command cmd{};
        cmd.type = bw::CommandType::RequestSync;  // marker: must survive untouched
        cmd.arg0 = 0xDEADBEEFu;
        TEST_ASSERT_FALSE(router.route(ctxs[i], rec, cmd));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::CommandType::RequestSync),
                              static_cast<int>(cmd.type));
        TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, cmd.arg0);
    }
}

}  // namespace

// Registration entry point called by main.cpp (one RUN_TEST per D2-N cell).
void register_input_router_tests(void) {
    RUN_TEST(test_route_idle_big_short);
    RUN_TEST(test_route_idle_big_long);
    RUN_TEST(test_route_idle_small_short);
    RUN_TEST(test_route_idle_small_long);
    RUN_TEST(test_route_idle_chord);
    RUN_TEST(test_route_alert_big_short);
    RUN_TEST(test_route_alert_big_long);
    RUN_TEST(test_route_alert_small_short);
    RUN_TEST(test_route_alert_small_long);
    RUN_TEST(test_route_alert_chord);
    RUN_TEST(test_route_prompt_big_short);
    RUN_TEST(test_route_prompt_big_long);
    RUN_TEST(test_route_prompt_small_short);
    RUN_TEST(test_route_prompt_small_long);
    RUN_TEST(test_route_prompt_chord);
    RUN_TEST(test_route_menu_big_short);
    RUN_TEST(test_route_menu_big_long);
    RUN_TEST(test_route_menu_small_short);
    RUN_TEST(test_route_menu_small_long);
    RUN_TEST(test_route_menu_chord);
    RUN_TEST(test_route_table_complete_no_duplicates);
    RUN_TEST(test_route_no_command_cells_stay_unrouted);
}
