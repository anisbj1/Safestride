// bw_ui — wiring sanity (DoD): wire_ui.cpp must COMPILE in the host build
// (the ARDUINO-guarded task body is covered by pio run -e esp32dev). The
// include compiles the whole wiring TU exactly as the composition root will,
// the D4 port is smoke-tested (push into uiCommands() must be accepted —
// W2: nothing routes yet on the consumer side, an empty queue is the default
// state, §9), and the wire()/binding invariant is regression-tested below
// (audit Low-2: no dangling bindings may survive a test).

#include <unity.h>

#include "../../../src/app/wiring/wire_ui.cpp"

#include "ui_fixtures.h"

namespace {

// f12::wire() stores POINTERS into its AppContext in the file-static s_inputs
// (wire_ui.cpp) and nothing ever unbinds them, so the context it is given
// must outlive every later f12 host test: program-lifetime storage, never a
// stack frame that dies when the test returns — a later test stepping the
// f12 model would read dead memory (UB).
app::AppContext& longLivedCtx() {
    static app::AppContext ctx{};
    return ctx;
}

}  // namespace

void test_wire_ui_host_port_accepts_commands(void) {
    bw::ICommandSink* sink = f12::uiCommands();
    TEST_ASSERT_NOT_NULL(sink);
    TEST_ASSERT_TRUE(sink->push(bw_ui_test::command(bw::CommandType::NextScreen)));
    TEST_ASSERT_TRUE(sink->push(bw_ui_test::command(bw::CommandType::OpenMenu)));
}

void test_wire_ui_host_wire_binds_context(void) {
    app::AppContext& ctx = longLivedCtx();  // host path: binds mailboxes, creates no task (D5)
    f12::wire(ctx);
    f12::wire(ctx);  // rebind is idempotent: plain pointer reassignment
}

// Audit Low-2 regression (dangling wire() binding). MUST run right after
// test_wire_ui_host_wire_binds_context — main.cpp registers it there.
// (a) The bindings inherited from that test must still point into the
//     program-lifetime context; a stack ctx there dies at return and fails
//     these pointer equalities deterministically.
// (b) wire() a SECOND live context and step the f12-owned model through the
//     rebind: a stale binding keeps reading the first ctx's data.
void test_wire_ui_rewire_binds_second_live_context(void) {
    const app::AppContext& first = longLivedCtx();
    TEST_ASSERT_EQUAL_PTR(&first.state_snapshot, f12::s_inputs.state);
    TEST_ASSERT_EQUAL_PTR(&first.alert_status, f12::s_inputs.alert);
    TEST_ASSERT_EQUAL_PTR(&first.ppg_features, f12::s_inputs.ppg);
    TEST_ASSERT_EQUAL_PTR(first.clock, f12::s_inputs.clock);

    static app::AppContext second{};  // also program-lifetime: nothing dangles after this test
    longLivedCtx().ppg_features.set(bw_ui_test::ppgHr(72.0f, 0));
    second.state_snapshot.set(bw_ui_test::snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    second.ppg_features.set(bw_ui_test::ppgHr(88.0f, 0));
    f12::wire(second);

    const bw::ui::UiFrame frame = f12::s_model.step(f12::s_inputs, 0);
    TEST_ASSERT_TRUE(frame.home.hr_valid);
    TEST_ASSERT_EQUAL_UINT16(88, frame.home.hr_bpm);
}

// --- Menu closure seam (backlog 32) -----------------------------------------
// These drive the exact code path uiTask runs (pumpInputs is outside the
// ARDUINO guard), through the two ports f11 posts to and the one it reads.
// The seams share file-static state with the tests above, so each case starts
// from a known screen and drains any staged action before it asserts.

namespace menu_seam {

// Returns the model to Home with no menu and no staged action pending.
//
// The queues are FILE-STATIC and shared with the wiring-sanity tests above,
// which push commands they never drain (test_wire_ui_host_port_accepts_commands
// leaves an OpenMenu behind). Draining them here is what keeps this block
// order-independent — without it the first pump below would re-open the menu
// this function just closed.
void resetMenuState() {
    bw::Command stale_cmd{};
    while (f12::s_command_queue.pop(stale_cmd)) {
    }
    f12::UiAction stale_action{};
    while (f12::s_action_queue.pop(stale_action)) {
    }
    f12::s_model.screens().activateScreen(bw::ui::ScreenId::Home);
    (void)f12::takeMenuAction();
}

}  // namespace menu_seam

// Boot default: no gesture queued, no menu open. f11 reads Idle from this.
void test_menu_seam_default_is_closed(void) {
    menu_seam::resetMenuState();
    f12::pumpInputs(0);
    TEST_ASSERT_FALSE(f12::menuOpen());
}

// OpenMenu (a Command on the existing D4 port) then a Select gesture: the
// gesture reaches the ScreenManager and picks entry 0 = Diagnostics.
void test_menu_seam_select_opens_diagnostics(void) {
    menu_seam::resetMenuState();
    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::OpenMenu)));
    f12::pumpInputs(0);  // apply the OpenMenu
    TEST_ASSERT_TRUE(f12::menuOpen());

    f12::postUiAction(f12::UiAction::Select);
    f12::pumpInputs(0);

    TEST_ASSERT_FALSE(f12::menuOpen());  // menuSelect() closes the menu
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::OpenDiagnostics),
                           static_cast<int>(f12::takeMenuAction()));
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::None),
                           static_cast<int>(f12::takeMenuAction()));  // drained once
}

// The menu-state port is what f11 derives InputContext::Menu from, so it must
// flip true the moment the menu opens and false again once a selection closes
// it — otherwise f11 keeps routing as Idle and the menu is unreachable.
void test_menu_seam_menu_open_tracks_screen(void) {
    menu_seam::resetMenuState();
    TEST_ASSERT_FALSE(f12::menuOpen());

    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::OpenMenu)));
    f12::pumpInputs(0);
    TEST_ASSERT_TRUE(f12::menuOpen());

    f12::postUiAction(f12::UiAction::Back);
    f12::pumpInputs(0);
    TEST_ASSERT_FALSE(f12::menuOpen());
}

// Selecting "Ack alerts" (entry 2) stages AcknowledgeAlert, which the f11 side
// converts to a Command. This asserts the ACTION crosses — never a menu index —
// so F11 stays ignorant of menu contents (F11 §12).
void test_menu_seam_ack_stages_action(void) {
    menu_seam::resetMenuState();
    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::OpenMenu)));
    f12::pumpInputs(0);

    // Scroll to entry 2 via the D2-N Small short path (NextScreen moves the
    // cursor while the menu is open — D7).
    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::NextScreen)));
    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::NextScreen)));
    f12::pumpInputs(0);

    f12::postUiAction(f12::UiAction::Select);
    f12::pumpInputs(0);

    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::MenuAction::AcknowledgeAlert),
                           static_cast<int>(f12::takeMenuAction()));
}

// Back from the menu restores the screen it was opened from, so the exit path
// can never strand the user (backlog 32's core trap).
void test_menu_seam_back_restores_previous_screen(void) {
    menu_seam::resetMenuState();
    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::NextScreen)));
    f12::pumpInputs(0);  // Home -> Diagnostics

    TEST_ASSERT_TRUE(f12::uiCommands()->push(bw_ui_test::command(bw::CommandType::OpenMenu)));
    f12::pumpInputs(0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(f12::s_model.step(f12::s_inputs, 0).screen));

    f12::postUiAction(f12::UiAction::Back);
    f12::pumpInputs(0);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Diagnostics),
                           static_cast<int>(f12::s_model.step(f12::s_inputs, 0).screen));
}
