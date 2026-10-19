// bw_ui — F12 §9 test_screen_manager: the pure screen cycle, menu and command
// routing (D3/D4/D6). Driven through UiModel so the frame contract is tested
// at the same time; commands arrive exactly as wire_ui delivers them.

#include <unity.h>

#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw_ui_test::command;
using CommandType = bw::CommandType;

namespace {

bw::ui::UiModel newModel() {
    return bw::ui::UiModel{};
}

}  // namespace

void test_screen_manager_first_step_is_home_and_powered(void) {
    bw::ui::UiModel model = newModel();
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_TRUE(frame.power);  // first-call state: on until the timeout
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
    TEST_ASSERT_FALSE(frame.banner.visible);
}

void test_screen_manager_empty_command_queue_changes_nothing(void) {
    bw::ui::UiModel model = newModel();
    (void)model.step(bw::ui::UiInputs{}, 0);
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 100);  // no commands
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_cycle_home_diag_home(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::NextScreen), 0);
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Diagnostics),
                           static_cast<int>(frame.screen));
    model.handleCommand(command(CommandType::NextScreen), 0);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_prompt_skipped_while_unavailable(void) {
    bw::ui::UiModel model = newModel();
    TEST_ASSERT_FALSE(model.screens().promptAvailable());  // D6 W2 default
    for (int i = 0; i < 10; ++i) {
        model.handleCommand(command(CommandType::NextScreen), 0);
        const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
        TEST_ASSERT_TRUE(frame.screen == bw::ui::ScreenId::Home ||
                         frame.screen == bw::ui::ScreenId::Diagnostics);
    }
}

void test_screen_manager_prompt_enters_cycle_when_available(void) {
    bw::ui::UiModel model = newModel();
    model.screens().setPromptAvailable(true);
    model.handleCommand(command(CommandType::NextScreen), 0);
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Prompt),
                           static_cast<int>(frame.screen));
    model.handleCommand(command(CommandType::NextScreen), 0);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Diagnostics),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_open_menu_and_back_returns(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::OpenMenu), 0);
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(frame.screen));
    TEST_ASSERT_EQUAL_UINT(0, frame.menu.selected);
    model.screens().menuBack();
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_open_menu_over_diagnostics_back_returns_there(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::NextScreen), 0);  // -> Diagnostics
    model.handleCommand(command(CommandType::OpenMenu), 0);
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(frame.screen));
    model.screens().menuBack();
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Diagnostics),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_next_screen_moves_menu_cursor(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::OpenMenu), 0);
    model.handleCommand(command(CommandType::NextScreen), 0);
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(frame.screen));
    TEST_ASSERT_EQUAL_UINT(1, frame.menu.selected);
    model.handleCommand(command(CommandType::NextScreen), 0);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(2, frame.menu.selected);
    model.handleCommand(command(CommandType::NextScreen), 0);  // wraps (forgiving input)
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(0, frame.menu.selected);
}

void test_screen_manager_menu_select_stages_semantic_action(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::OpenMenu), 0);
    model.screens().menuSelect();  // entry 0 = Diagnostics
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Diagnostics),
                           static_cast<int>(frame.screen));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::OpenDiagnostics),
                           static_cast<int>(model.takeAction()));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::None),
                           static_cast<int>(model.takeAction()));  // drained once
}

void test_screen_manager_menu_ack_stages_acknowledge_alert(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::OpenMenu), 0);
    model.screens().menuMoveSelection();
    model.screens().menuMoveSelection();  // entry 2 = Ack alerts
    model.screens().menuSelect();
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    // §13: the selection stages the semantic action; no Command is pushed.
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::AcknowledgeAlert),
                           static_cast<int>(model.takeAction()));
}

void test_screen_manager_non_ui_commands_are_ignored(void) {
    bw::ui::UiModel model = newModel();
    model.handleCommand(command(CommandType::AcknowledgeAlert), 0);  // F10's pump owns it
    model.handleCommand(command(CommandType::StartNap), 0);
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
}

void test_screen_manager_null_inputs_render_honest_defaults(void) {
    bw::ui::UiModel model = newModel();
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_FALSE(frame.home.snapshot_valid);
    TEST_ASSERT_FALSE(frame.home.hr_valid);
    TEST_ASSERT_FALSE(frame.home.battery_valid);
    TEST_ASSERT_FALSE(frame.diag.available);
}
