// bw_ui — F12 §9 test_alert_overlay: D3's overlay priority and min-show
// bookkeeping on FakeClock timelines, plus the D8 alert wake.

#include <cstdio>
#include <cstring>

#include <unity.h>

#include <bw/containers/latest_value.h>
#include <bw/testkit/fake_canvas.h>
#include <bw/testkit/fake_clock.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw::testkit::FakeClock;
using bw_ui_test::alert;
using bw_ui_test::command;
using bw_ui_test::loadGolden;
using bw_ui_test::snapshot;
using CommandType = bw::CommandType;

namespace {

struct OverlayRig {
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::LatestValue<bw::StateSnapshot> state_mailbox;
    bw::ui::UiInputs inputs;
    bw::ui::UiModel model{};

    OverlayRig() {
        inputs.alert = &alert_mailbox;
        inputs.state = &state_mailbox;
        state_mailbox.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    }

    bw::ui::UiFrame raise(bw::AlertLevel level, bw::TimeMs now, bool acknowledged = false) {
        alert_mailbox.set(alert(level, acknowledged));
        return model.step(inputs, now);
    }
    bw::ui::UiFrame stepAt(bw::TimeMs now) { return model.step(inputs, now); }
};

}  // namespace

void test_demo_fullscreen_centered_blink_and_immediate_clear(void) {
    auto config = bw::ui::defaultUiConfig();
    config.demo_wake_protocol = true;
    bw::ui::UiModel model{config};
    bw::LatestValue<bw::AlertStatus> mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &mailbox;
    model.handleCommand(command(CommandType::OpenMenu), 0);
    bw::AlertStatus status{};
    status.t_ms = 1000;
    status.level = bw::AlertLevel::Warning;
    mailbox.set(status);
    bw::ui::ScreenRenderer renderer;
    FakeCanvas on;
    renderer.render(model.step(inputs, 1000), on, 1000);
    TEST_ASSERT_EQUAL_STRING("power on\nclear\ntext 29,22 large \"Drowsy?\"\nflush ok\n", on.log());
    FakeCanvas off;
    renderer.render(model.step(inputs, 1500), off, 1500);
    TEST_ASSERT_EQUAL_STRING("clear\nflush ok\n", off.log());
    status.t_ms = 1600;
    status.level = bw::AlertLevel::Active;
    mailbox.set(status);
    FakeCanvas red;
    renderer.render(model.step(inputs, 1600), red, 1600);
    TEST_ASSERT_EQUAL_STRING("clear\ntext 19,22 large \"Sleeping?\"\nflush ok\n", red.log());
    status.level = bw::AlertLevel::Normal;
    mailbox.set(status);
    auto frame = model.step(inputs, 1700);
    TEST_ASSERT_FALSE(frame.banner.visible);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ui::ScreenId::Menu), static_cast<int>(frame.screen));
    FakeCanvas cleared;
    renderer.render(frame, cleared, 1700);
    TEST_ASSERT_NOT_NULL(std::strstr(cleared.log(), "Menu"));
    TEST_ASSERT_NULL(std::strstr(cleared.log(), "Sleeping?"));
}

void test_alert_overlay_warning_banner_over_home(void) {
    OverlayRig rig;
    bw::ui::UiFrame frame = rig.stepAt(100);
    TEST_ASSERT_FALSE(frame.banner.visible);

    frame = rig.raise(bw::AlertLevel::Warning, 200);
    TEST_ASSERT_TRUE(frame.power);           // D8: raise wakes/holds the screen
    TEST_ASSERT_TRUE(frame.banner.visible);  // D3: overlay is up
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Home),
                           static_cast<int>(frame.screen));  // ...over Home

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 200);
    // Overlay order: the home text is drawn first, the banner band last.
    // Band token updated with the backlog-21 redesign (mask = fillRect off,
    // 1 px rule closes it) — goldens banner_ack/frame_home_alert pin the
    // full op sequence; y semantics pinned by CR-013 (top-left).
    const char* const banner = std::strstr(canvas.log(), "rect 0,0 128,16 off");
    TEST_ASSERT_NOT_NULL(banner);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "rect 0,15 128,1 on"));  // separator rule
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "text 0,0 medium \"Awake\""));
    TEST_ASSERT_TRUE(std::strstr(canvas.log(), "text 0,0 medium \"Awake\"") < banner);
}

void test_alert_overlay_banner_over_menu(void) {
    OverlayRig rig;
    rig.model.handleCommand(command(CommandType::OpenMenu), 100);
    bw::ui::UiFrame frame = rig.stepAt(100);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(frame.screen));

    frame = rig.raise(bw::AlertLevel::Active, 200);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::ui::ScreenId::Menu),
                           static_cast<int>(frame.screen));  // priority: user's place kept
    TEST_ASSERT_TRUE(frame.banner.visible);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 200);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "text 0,0 medium \"Menu\""));  // under
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "Drowsy?"));                   // over
}

void test_alert_overlay_min_show_persists_after_normal(void) {
    OverlayRig rig;
    const bw::TimeMs kRaise = 1000;
    bw::ui::UiFrame frame = rig.raise(bw::AlertLevel::Warning, kRaise);
    TEST_ASSERT_TRUE(frame.banner.visible);

    // Level back to Normal one second later: the banner must linger for the
    // whole kAlertScreenMinShowMs window measured from the raise (D3).
    frame = rig.raise(bw::AlertLevel::Normal, kRaise + 1000);
    TEST_ASSERT_TRUE(frame.banner.visible);

    frame = rig.stepAt(kRaise + bw::ui::kAlertScreenMinShowMs - 1);
    TEST_ASSERT_TRUE(frame.banner.visible);

    frame = rig.stepAt(kRaise + bw::ui::kAlertScreenMinShowMs);
    TEST_ASSERT_FALSE(frame.banner.visible);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::AlertLevel::Normal),
                           static_cast<int>(frame.banner.level));
}

void test_alert_overlay_rearm_extends_min_show(void) {
    // A level that keeps re-raising pushes the deadline forward; when it
    // finally drops to Normal the LAST push governs (D3: no flicker).
    OverlayRig rig;
    (void)rig.raise(bw::AlertLevel::Warning, 0);
    (void)rig.raise(bw::AlertLevel::Active, 2000);
    bw::ui::UiFrame frame = rig.raise(bw::AlertLevel::Normal, 3000);
    TEST_ASSERT_TRUE(frame.banner.visible);
    frame = rig.stepAt(4999);  // deadline = last push (2000) + 3000
    TEST_ASSERT_TRUE(frame.banner.visible);
    frame = rig.stepAt(5000);
    TEST_ASSERT_FALSE(frame.banner.visible);
}

void test_alert_overlay_persistent_level_keeps_banner(void) {
    OverlayRig rig;
    (void)rig.raise(bw::AlertLevel::Persistent, 0);
    bw::ui::UiFrame frame = rig.stepAt(60000);  // an hour later: still Persistent
    TEST_ASSERT_TRUE(frame.banner.visible);
    TEST_ASSERT_EQUAL_UINT(static_cast<int>(bw::AlertLevel::Persistent),
                           static_cast<int>(frame.banner.level));
}

void test_alert_overlay_acknowledged_renders_ack_golden(void) {
    // Alert-only inputs (no state mailbox): the home part shows "No data",
    // so this golden isolates the banner-over-idle-screen render.
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &alert_mailbox;
    bw::ui::UiModel model{};
    const bw::ui::UiFrame frame = model.step(inputs, 0);
    alert_mailbox.set(alert(bw::AlertLevel::Warning, true));

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(model.step(inputs, 1000), canvas, 1000);
    TEST_ASSERT_TRUE(frame.power);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "Check? ACK"));

    char golden[2048];
    // Updated with the backlog-21 banner redesign (erase-mask + 1 px rule,
    // y unchanged = top-left per CR-013); expectation updated, not weakened —
    // the golden still byte-compares the whole frame.
    loadGolden("banner_ack", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

void test_alert_overlay_warning_and_active_wording(void) {
    OverlayRig rig;
    FakeCanvas warning_canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(rig.raise(bw::AlertLevel::Warning, 100), warning_canvas, 100);
    TEST_ASSERT_NOT_NULL(std::strstr(warning_canvas.log(), "Check?\""));
    TEST_ASSERT_NULL(std::strstr(warning_canvas.log(), "Warning\""));

    FakeCanvas active_canvas;
    renderer.render(rig.raise(bw::AlertLevel::Active, 200), active_canvas, 200);
    TEST_ASSERT_NOT_NULL(std::strstr(active_canvas.log(), "Drowsy?\""));
    TEST_ASSERT_NULL(std::strstr(active_canvas.log(), "asleep"));
}

void test_alert_overlay_alert_wakes_timed_out_display(void) {
    // Tiny timeout so the timeline stays short (UiConfig knobs, D8).
    bw::ui::UiConfig config = bw::ui::defaultUiConfig();
    config.display_timeout_ms = 500;
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &alert_mailbox;
    bw::ui::UiModel model{config};

    bw::ui::UiFrame frame = model.step(inputs, 0);
    TEST_ASSERT_TRUE(frame.power);
    frame = model.step(inputs, 600);  // past the timeout: off
    TEST_ASSERT_FALSE(frame.power);

    alert_mailbox.set(alert(bw::AlertLevel::Warning));
    frame = model.step(inputs, 700);  // D8: alert raise wakes the display
    TEST_ASSERT_TRUE(frame.power);
    TEST_ASSERT_TRUE(frame.banner.visible);
}

void test_alert_overlay_banner_golden_over_home(void) {
    OverlayRig rig;
    (void)rig.stepAt(0);  // home screen first, banner second (two-pass draw)
    const bw::ui::UiFrame frame = rig.raise(bw::AlertLevel::Warning, 100, true);

    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(frame, canvas, 100);

    char golden[2048];
    loadGolden("frame_home_alert", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

void test_alert_overlay_banner_text_fits_offset_cap(void) {
    // Audit F-2: the banner text starts at kBannerTextXPx (right of the
    // warning icon), so every level's text must be truncated to the
    // offset-aware kMaxLineCharsBanner — never drawn past the 128 px panel.
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::ui::UiInputs inputs;
    inputs.alert = &alert_mailbox;
    bw::ui::UiModel model{};

    char needle[48];
    std::snprintf(needle, sizeof(needle), "text %d,%d medium \"",
                  static_cast<int>(bw::ui::kBannerTextXPx), 0);

    const bw::AlertLevel kLevels[] = {bw::AlertLevel::Warning, bw::AlertLevel::Active,
                                      bw::AlertLevel::Persistent};
    for (unsigned ack = 0; ack <= 1; ++ack) {
        for (const bw::AlertLevel level : kLevels) {
            alert_mailbox.set(alert(level, ack != 0));
            FakeCanvas canvas;
            bw::ui::ScreenRenderer renderer;
            renderer.render(model.step(inputs, 1000), canvas, 1000);

            const char* start = std::strstr(canvas.log(), needle);
            TEST_ASSERT_NOT_NULL_MESSAGE(start, "the banner text row must be drawn");
            start += std::strlen(needle);
            const char* const end = std::strchr(start, '"');
            TEST_ASSERT_NOT_NULL(end);
            TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(
                static_cast<unsigned>(bw::ui::kMaxLineCharsBanner),
                static_cast<unsigned>(end - start), "banner text must fit its offset-aware cap");
        }
    }

    // Worst case in full: "Persistent" + " ACK" (14 chars) truncates to the
    // 13-char cap; a shorter acked line ("Warning ACK", 11 chars) is NOT
    // truncated (also pinned by the banner_ack golden).
    alert_mailbox.set(alert(bw::AlertLevel::Persistent, true));
    FakeCanvas canvas;
    bw::ui::ScreenRenderer renderer;
    renderer.render(model.step(inputs, 2000), canvas, 2000);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "\"Persistent AC\""));
    TEST_ASSERT_NULL(std::strstr(canvas.log(), "\"Persistent ACK\""));
}
