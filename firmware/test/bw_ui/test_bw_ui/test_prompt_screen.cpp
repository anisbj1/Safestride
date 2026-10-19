// bw_ui — F12 §9 test_prompt_screen: D6's degraded placeholder render, its
// golden, and the availability flip that re-enters Prompt into the cycle
// without any other change. Extended by backlog 27: the real prompt screen
// rendering CR-016's ConfirmPromptState (seq / phase / deadline / depth).

#include <cstring>

#include <unity.h>

#include <bw/testkit/fake_canvas.h>
#include <bw/testkit/fake_clock.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw::testkit::FakeClock;
using bw_ui_test::command;
using bw_ui_test::loadGolden;
using CommandType = bw::CommandType;

namespace {

// Renders the Prompt screen directly (the D6 gate decides cycle membership,
// not whether the screen may be shown) and returns the canvas log.
void renderPromptAt(const bw::ui::UiInputs& inputs, bw::TimeMs now, FakeCanvas& canvas) {
    bw::ui::UiModel model{};
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);
    const bw::ui::UiFrame frame = model.step(inputs, now);
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, now);
}

}  // namespace

void test_prompt_screen_placeholder_golden(void) {
    bw::ui::UiModel model{};
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);  // direct activation (§9)
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);

    char golden[2048];
    loadGolden("prompt_placeholder", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

void test_prompt_screen_availability_flip_only_changes_cycle(void) {
    bw::ui::UiModel model{};
    TEST_ASSERT_FALSE(model.screens().promptAvailable());
    model.screens().setPromptAvailable(true);
    // "without other changes": the active screen and the first cycle step
    // behave exactly as with the flip absent, except Prompt now appears.
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));
    model.handleCommand(command(CommandType::NextScreen), 0);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Prompt),
                           static_cast<int>(frame.screen));
}

void test_prompt_screen_direct_activation_while_unavailable_renders_placeholder(void) {
    bw::ui::UiModel model{};
    TEST_ASSERT_FALSE(model.screens().promptAvailable());
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Prompt),
                           static_cast<int>(frame.screen));

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "No prompts"));
}

// --- Backlog 27: the real prompt screen (CR-016) -----------------------------

// Offered session, clock wired: every field F14 publishes is on the panel —
// the ask, the event seq, the reply countdown and the queue behind it.
void test_prompt_screen_offered_renders_all_fields(void) {
    FakeClock clock;
    clock.setMs(5000);
    bw::ui::UiInputs inputs;
    const bw_ui_test::FakePromptProvider provider = bw_ui_test::FakePromptProvider::offered(
        /*seq=*/42, /*now=*/5000, /*seconds_left=*/8, /*queue_depth=*/2);
    inputs.prompt = &provider;
    inputs.clock = &clock;

    FakeCanvas canvas;
    renderPromptAt(inputs, 5000, canvas);

    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "Confirm?"));
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "event 42"));
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "reply in 8s"));
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "queued 2"));
}

// The countdown is real arithmetic, not a constant: advancing the clock must
// shrink the rendered seconds.
void test_prompt_screen_countdown_follows_clock(void) {
    FakeClock clock;
    clock.setMs(1000);
    bw_ui_test::FakePromptProvider provider =
        bw_ui_test::FakePromptProvider::offered(/*seq=*/7, /*now=*/1000, /*seconds_left=*/30);
    bw::ui::UiInputs inputs;
    inputs.prompt = &provider;
    inputs.clock = &clock;

    bw::ui::UiModel model{};
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);

    bw::ui::UiFrame frame = model.step(inputs, 1000);
    TEST_ASSERT_TRUE(frame.prompt.deadline_valid);
    TEST_ASSERT_EQUAL_UINT16(30, frame.prompt.seconds_left);

    clock.setMs(1000 + 9000);  // 9 s elapsed
    frame = model.step(inputs, clock.nowMs());
    TEST_ASSERT_EQUAL_UINT16(21, frame.prompt.seconds_left);
}

// A Deferred session has no deadline by contract (0 unless Offered), so the
// countdown renders "--" — an honest gap, never a guessed number (§10).
void test_prompt_screen_deferred_has_no_countdown(void) {
    FakeClock clock;
    clock.setMs(0);
    bw::ui::UiInputs inputs;
    const bw_ui_test::FakePromptProvider provider = bw_ui_test::FakePromptProvider::deferred(9);
    inputs.prompt = &provider;
    inputs.clock = &clock;

    bw::ui::UiModel model{};
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);
    const bw::ui::UiFrame frame = model.step(inputs, 0);

    TEST_ASSERT_TRUE(frame.prompt.available);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Deferred),
                          static_cast<int>(frame.prompt.phase));
    TEST_ASSERT_FALSE(frame.prompt.deadline_valid);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "Deferred"));
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "reply --"));
}

// No clock wired (D5): `now` is a fabricated 0, so a countdown computed from
// it would be fiction. The screen says "--" instead.
void test_prompt_screen_without_clock_renders_unknown_countdown(void) {
    bw::ui::UiInputs inputs;  // clock left null
    const bw_ui_test::FakePromptProvider provider = bw_ui_test::FakePromptProvider::offered(
        /*seq=*/3, /*now=*/0, /*seconds_left=*/5000);
    inputs.prompt = &provider;

    bw::ui::UiModel model{};
    model.screens().activateScreen(bw::ui::ScreenId::Prompt);
    const bw::ui::UiFrame frame = model.step(inputs, 0);

    TEST_ASSERT_TRUE(frame.prompt.available);  // the ask is still real
    TEST_ASSERT_FALSE(frame.prompt.deadline_valid);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 0);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "reply --"));
}

// Golden: the real prompt screen, all four rows. Pins the layout — headline
// in Medium (the ask), the three facts in Small — so a future reflow has to
// update this file deliberately rather than drift.
void test_prompt_screen_offered_golden(void) {
    FakeClock clock;
    clock.setMs(1000);
    bw::ui::UiInputs inputs;
    const bw_ui_test::FakePromptProvider provider = bw_ui_test::FakePromptProvider::offered(
        /*seq=*/42, /*now=*/1000, /*seconds_left=*/8, /*queue_depth=*/2);
    inputs.prompt = &provider;
    inputs.clock = &clock;

    FakeCanvas canvas;
    renderPromptAt(inputs, 1000, canvas);

    char golden[2048];
    loadGolden("prompt_offered", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

// Two "no session" shapes must BOTH render the placeholder: a provider that
// has never published (latest() == false) and one reporting phase Idle (the
// session ended — withdrawn, timed out or answered).
void test_prompt_screen_no_session_renders_placeholder(void) {
    bw::ui::UiInputs inputs;

    const bw_ui_test::FakePromptProvider empty = bw_ui_test::FakePromptProvider::empty();
    inputs.prompt = &empty;
    FakeCanvas empty_canvas;
    renderPromptAt(inputs, 0, empty_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(empty_canvas.log(), "No prompts"));

    const bw_ui_test::FakePromptProvider idle = bw_ui_test::FakePromptProvider::idle();
    inputs.prompt = &idle;
    FakeCanvas idle_canvas;
    renderPromptAt(inputs, 0, idle_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(idle_canvas.log(), "No prompts"));
}

// The D6 gate is driven by the SAME read that fills the frame: a live session
// re-enters Prompt into the NextScreen cycle, and an ended one removes it
// again — no second poll to keep in step (backlog 27 moved the gate into the
// model on purpose).
void test_prompt_screen_provider_drives_availability_gate(void) {
    FakeClock clock;
    clock.setMs(0);
    bw::ui::UiInputs inputs;

    bw::ui::UiModel model{};
    const bw_ui_test::FakePromptProvider offered =
        bw_ui_test::FakePromptProvider::offered(/*seq=*/1, /*now=*/0, /*seconds_left=*/10);
    inputs.prompt = &offered;
    inputs.clock = &clock;

    (void)model.step(inputs, 0);
    TEST_ASSERT_TRUE(model.screens().promptAvailable());

    // Session ends (F14 returns to Idle): the gate closes on the next frame.
    const bw_ui_test::FakePromptProvider idle = bw_ui_test::FakePromptProvider::idle();
    inputs.prompt = &idle;
    (void)model.step(inputs, 0);
    TEST_ASSERT_FALSE(model.screens().promptAvailable());
}

// A session that has never been seen (no provider bound at all) must keep the
// W2 default exactly: gate closed, placeholder rendered.
void test_prompt_screen_absent_provider_keeps_w2_default(void) {
    bw::ui::UiModel model{};
    const bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_FALSE(model.screens().promptAvailable());
    TEST_ASSERT_FALSE(frame.prompt.available);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ConfirmPromptPhase::Idle),
                          static_cast<int>(frame.prompt.phase));
}
