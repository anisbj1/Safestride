// bw_ui — F12 §14 golden set: one canonical frame per screen, byte-compared
// against test/bw_ui/golden/*.log. Home, Prompt and Diagnostics goldens live
// in their per-screen files (§9), the alert-over-Home frame in
// test_alert_overlay.cpp; the menu canonical frame lives here.
// y values in every golden are top-left of the glyph box (CR-013, pinned
// contract semantics): F12 always composed top-left, so the non-banner
// goldens are byte-identical after the pin — only the banner goldens moved
// (backlog 21 redesign, see test_alert_overlay.cpp).

#include <unity.h>

#include <bw/containers/latest_value.h>
#include <bw/testkit/fake_canvas.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw_ui_test::command;
using bw_ui_test::loadGolden;
using CommandType = bw::CommandType;

void test_golden_menu_canonical(void) {
    bw::ui::UiModel model{};
    model.handleCommand(command(CommandType::OpenMenu), 0);  // cursor on entry 0
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);

    char golden[2048];
    loadGolden("menu_canonical", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

void test_golden_menu_cursor_position(void) {
    // Second entry highlighted: the cursor row must move with the selection.
    bw::ui::UiModel model{};
    model.handleCommand(command(CommandType::OpenMenu), 0);
    model.screens().menuMoveSelection();
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);

    char golden[2048];
    loadGolden("menu_cursor_second", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}
