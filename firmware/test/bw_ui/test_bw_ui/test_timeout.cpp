// bw_ui — F12 §9 test_timeout: D8's power policy on FakeClock timelines —
// frames stop at kDisplayTimeoutMs, commands reset the timer, an alert raise
// wakes the screen, and no clock at all disables the timeout (D5).

#include <unity.h>

#include <bw/containers/latest_value.h>
#include <bw/testkit/fake_canvas.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw_ui_test::alert;
using bw_ui_test::command;
using CommandType = bw::CommandType;

namespace {

// [EXPERIMENTAL] Short timeout for compact timelines; the production default
// is kDisplayTimeoutMs (30 s since 2026-10-15), the behavior under test is
// identical.
bw::ui::UiConfig shortTimeoutConfig() {
    bw::ui::UiConfig config = bw::ui::defaultUiConfig();
    config.display_timeout_ms = 500;
    return config;
}

// [EXPERIMENTAL] Timeout deliberately LONGER than the min-show window (audit
// F-1): with timeout < min-show, reset and no-reset converge at the moment
// the banner drops, so only timeout > min-show isolates D8's "the alert
// raise resets the inactivity timer" claim.
bw::ui::UiConfig timeoutOutlivesMinShowConfig() {
    bw::ui::UiConfig config = bw::ui::defaultUiConfig();
    config.display_timeout_ms = 5000;  // > config.alert_min_show_ms (3000)
    return config;
}

}  // namespace

void test_timeout_power_off_after_inactivity(void) {
    bw::ui::UiModel model{shortTimeoutConfig()};
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 0).power);
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 499).power);
    TEST_ASSERT_FALSE(model.step(bw::ui::UiInputs{}, 500).power);   // exactly at the cap
    TEST_ASSERT_FALSE(model.step(bw::ui::UiInputs{}, 5000).power);  // stays off
}

void test_timeout_command_resets_timer(void) {
    bw::ui::UiModel model{shortTimeoutConfig()};
    (void)model.step(bw::ui::UiInputs{}, 400);
    model.handleCommand(command(CommandType::NextScreen), 400);
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 899).power);
    TEST_ASSERT_FALSE(model.step(bw::ui::UiInputs{}, 900).power);  // 400 + 500
}

void test_timeout_alert_raise_wakes_and_resets_timer(void) {
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &alert_mailbox;
    bw::ui::UiModel model{shortTimeoutConfig()};

    (void)model.step(inputs, 600);
    TEST_ASSERT_FALSE(model.step(inputs, 600).power);  // timed out

    alert_mailbox.set(alert(bw::AlertLevel::Warning));
    bw::ui::UiFrame frame = model.step(inputs, 700);
    TEST_ASSERT_TRUE(frame.power);  // wake
    TEST_ASSERT_TRUE(frame.banner.visible);

    // Alert returns to Normal: the banner holds for kAlertScreenMinShowMs,
    // and only then does the (reset) timeout take effect again.
    alert_mailbox.set(alert(bw::AlertLevel::Normal));
    frame = model.step(inputs, 3699);  // 700 + 3000 - 1: last banner moment
    TEST_ASSERT_TRUE(frame.power);
    TEST_ASSERT_TRUE(frame.banner.visible);
    frame = model.step(inputs, 3700);  // exactly the deadline (3700 - 700 >= 500)
    TEST_ASSERT_FALSE(frame.banner.visible);
    TEST_ASSERT_FALSE(frame.power);
}

void test_timeout_alert_reset_outlives_banner_min_show(void) {
    // Audit F-1 (D8 isolating test): display_timeout_ms (5000) >
    // alert_min_show_ms (3000). After the alert de-asserts, the banner drops
    // at raise+3000, but power must hold until raise+5000 — only the reset
    // of last_activity_ms_ at the raise makes that true.
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &alert_mailbox;
    bw::ui::UiModel model{timeoutOutlivesMinShowConfig()};

    (void)model.step(inputs, 0);
    TEST_ASSERT_TRUE(model.step(inputs, 4000).power);  // 4000 < 5000: still on

    const bw::TimeMs kRaise = 4500;
    alert_mailbox.set(alert(bw::AlertLevel::Warning));
    bw::ui::UiFrame frame = model.step(inputs, kRaise);  // raise: reset to 4500
    TEST_ASSERT_TRUE(frame.power);
    TEST_ASSERT_TRUE(frame.banner.visible);

    alert_mailbox.set(alert(bw::AlertLevel::Normal));
    frame = model.step(inputs, kRaise + 2999);  // banner holds for min-show
    TEST_ASSERT_TRUE(frame.banner.visible);
    TEST_ASSERT_TRUE(frame.power);

    frame = model.step(inputs, kRaise + 3000);  // banner gone; timeout from 4500
    TEST_ASSERT_FALSE(frame.banner.visible);
    TEST_ASSERT_TRUE_MESSAGE(frame.power, "an alert raise must reset the inactivity timer (D8)");

    frame = model.step(inputs, kRaise + 4999);  // one ms before the reset cap
    TEST_ASSERT_TRUE(frame.power);
    frame = model.step(inputs, kRaise + 5000);  // exactly 4500 + 5000
    TEST_ASSERT_FALSE(frame.power);
}

void test_timeout_renderer_stops_flushing_when_off(void) {
    bw::ui::UiModel model{shortTimeoutConfig()};
    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;

    renderer.render(model.step(bw::ui::UiInputs{}, 0), canvas, 0);
    TEST_ASSERT_TRUE(canvas.power());
    const unsigned flushes_while_on = static_cast<unsigned>(canvas.flushCount());

    renderer.render(model.step(bw::ui::UiInputs{}, 600), canvas, 600);
    TEST_ASSERT_FALSE(canvas.power());
    // D8: powered off = no draw, no flush (bus fully quiet), one power edge.
    TEST_ASSERT_EQUAL_UINT(flushes_while_on, static_cast<unsigned>(canvas.flushCount()));
}

void test_timeout_disabled_without_clock(void) {
    // D5: no clock ⇒ the task passes 0 every tick; no age can grow, so the
    // screen never times out and staleness is disabled — the honest default
    // until the integrator wires an IClock.
    bw::ui::UiModel model{shortTimeoutConfig()};
    bw::ui::UiFrame frame = model.step(bw::ui::UiInputs{}, 0);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_TRUE(frame.power);
    frame = model.step(bw::ui::UiInputs{}, 0);
    TEST_ASSERT_TRUE(frame.power);
}

void test_timeout_zero_disables_sleep(void) {
    // display_timeout_ms = 0 is the "never sleep" sentinel (testing builds):
    // no cap is configured, so arbitrarily long idleness keeps power on.
    bw::ui::UiConfig config = bw::ui::defaultUiConfig();
    config.display_timeout_ms = 0;
    bw::ui::UiModel model{config};
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 0).power);
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 60000).power);
    TEST_ASSERT_TRUE(model.step(bw::ui::UiInputs{}, 3600000).power);  // 1 h idle
}
