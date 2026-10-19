// bw_ui — F12 §9 test_home_screen: golden renders per state plus the honest
// "--" degradation for invalid HR, stale PPG and the A-04 absent battery.

#include <cstring>
#include <initializer_list>
#include <limits>

#include <unity.h>

#include <bw/containers/latest_value.h>
#include <bw/testkit/fake_canvas.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw_ui_test::FakeBattery;
using bw_ui_test::loadGolden;
using bw_ui_test::ppgHr;
using bw_ui_test::ppgHrInvalid;
using bw_ui_test::snapshot;

namespace {

// Renders one home-screen frame with the given sources and returns the log
// in `out` (canvas kept alive by the caller through the pointer).
struct HomeRig {
    bw::LatestValue<bw::StateSnapshot> state;
    bw::LatestValue<bw::PpgFeatures> ppg;
    FakeBattery battery;
    bw::ui::UiInputs inputs;
    bw::ui::UiModel model{};

    HomeRig() {
        inputs.state = &state;
        inputs.ppg = &ppg;
        inputs.battery = &battery;
    }

    void renderAt(bw::TimeMs now, FakeCanvas& canvas) {
        bw::ui::ScreenRenderer renderer;
        renderer.render(model.step(inputs, now), canvas, now);
    }
};

void assertGolden(const char* golden_name, const FakeCanvas& canvas) {
    char golden[2048];
    loadGolden(golden_name, golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
}

}  // namespace

void test_home_screen_golden_awake_worn(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 990));
    rig.battery = FakeBattery::atPercent(87.0f);

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    assertGolden("home_awake_worn", canvas);
}

void test_home_screen_golden_drowsy(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Drowsy, bw::WearState::Worn));
    rig.ppg.set(ppgHr(58.0f, 990));
    rig.battery = FakeBattery::atPercent(61.0f);

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    assertGolden("home_drowsy", canvas);
}

void test_home_screen_golden_not_worn(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::NotWorn, bw::WearState::NotWorn));
    rig.ppg.set(ppgHrInvalid(990));
    rig.battery = FakeBattery::atPercent(61.0f);

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    assertGolden("home_notworn", canvas);
}

void test_home_screen_golden_unknown_state(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Unknown, bw::WearState::Uncertain));
    rig.ppg.set(ppgHrInvalid(990));
    rig.battery = FakeBattery::absent();

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    assertGolden("home_unknown", canvas);
}

void test_home_screen_golden_no_snapshot_yet(void) {
    HomeRig rig;  // startup: every mailbox empty, no battery divider (A-04)
    FakeCanvas canvas;
    rig.renderAt(0, canvas);
    assertGolden("home_no_data", canvas);
}

void test_home_screen_hr_valid_rounds_to_integer(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.4f, 990));  // rounds to "72", never "72.4" (no %f)
    rig.battery = FakeBattery::atPercent(87.0f);

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR 72\""));
    TEST_ASSERT_NULL(std::strstr(canvas.log(), "72.4"));
}

void test_home_screen_hr_invalid_renders_dashes(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHrInvalid(990));  // bench: MAX30102 absent
    rig.battery = FakeBattery::atPercent(87.0f);

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR --"));
}

void test_home_screen_hr_good_and_degraded_worn_render_number(void) {
    for (const bw::DataQuality quality : {bw::DataQuality::Good, bw::DataQuality::Degraded}) {
        HomeRig rig;
        rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
        rig.ppg.set(ppgHr(72.0f, 990, quality));
        FakeCanvas canvas;
        rig.renderAt(1000, canvas);
        TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR 72\""));
    }
}

void test_home_screen_hr_insufficient_quality_renders_dashes(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 990, bw::DataQuality::Insufficient));
    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR --"));
}

void test_home_screen_hr_requires_fresh_worn_snapshot(void) {
    const bw::WearState wear_states[] = {bw::WearState::NotWorn, bw::WearState::Uncertain};
    for (const bw::WearState wear : wear_states) {
        HomeRig rig;
        rig.state.set(snapshot(bw::AlertnessState::Awake, wear));
        rig.ppg.set(ppgHr(72.0f, 990));
        FakeCanvas canvas;
        rig.renderAt(1000, canvas);
        TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR --"));
    }

    HomeRig missing;
    missing.ppg.set(ppgHr(72.0f, 990));
    FakeCanvas missing_canvas;
    missing.renderAt(1000, missing_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(missing_canvas.log(), "HR --"));

    HomeRig stale;
    stale.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    stale.ppg.set(ppgHr(72.0f, 1000 + bw::ui::kHrStaleMs + 1));
    FakeCanvas stale_canvas;
    stale.renderAt(1000 + bw::ui::kHrStaleMs + 1, stale_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(stale_canvas.log(), "HR --"));
}

void test_home_screen_stale_ppg_renders_dashes(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 0));  // t_ms = 0; now = kHrStaleMs + 1 is far past it
    rig.battery = FakeBattery::atPercent(87.0f);

    FakeCanvas canvas;
    rig.renderAt(bw::ui::kHrStaleMs + 1, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR --"));

    FakeCanvas fresh;
    rig.renderAt(bw::ui::kHrStaleMs, fresh);  // exactly at the cap: still shown (§8)
    TEST_ASSERT_NOT_NULL(std::strstr(fresh.log(), "HR 72\""));
}

void test_home_screen_battery_absent_renders_dashes(void) {
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 990));
    rig.battery = FakeBattery::absent();  // bench A-04: valid == false

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "\"--\""));
    TEST_ASSERT_NULL(std::strstr(canvas.log(), "%"));
}

void test_home_screen_forward_timestamp_is_not_stale(void) {
    // A producer timestamp ahead of `now` (defensive branch, ui_model.cpp)
    // must clamp the age to 0, not read as a huge unsigned age.
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 5000));
    rig.battery = FakeBattery::absent();

    FakeCanvas canvas;
    rig.renderAt(0, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR 72\""));
}

void test_home_screen_non_finite_hr_renders_dashes(void) {
    // Audit F-4: hr_valid == true with a NaN or NEGATIVE bpm (glitching
    // sensor/parser) must read "HR --" — rounding either to 0/999 would
    // fabricate a number from bad data (§10).
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.battery = FakeBattery::absent();

    rig.ppg.set(ppgHr(std::numeric_limits<float>::quiet_NaN(), 990));
    FakeCanvas nan_canvas;
    rig.renderAt(1000, nan_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(nan_canvas.log(), "HR --"));

    rig.ppg.set(ppgHr(-5.0f, 990));
    FakeCanvas negative_canvas;
    rig.renderAt(1000, negative_canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(negative_canvas.log(), "HR --"));
}

void test_home_screen_non_finite_battery_renders_dashes(void) {
    // Audit F-4: battery valid == true with a non-finite estimate must read
    // "--", never a fabricated "0%" or "999%".
    HomeRig rig;
    rig.state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    rig.ppg.set(ppgHr(72.0f, 990));
    rig.battery = FakeBattery::atPercent(std::numeric_limits<float>::infinity());

    FakeCanvas canvas;
    rig.renderAt(1000, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "HR 72\""));
    TEST_ASSERT_NULL(std::strstr(canvas.log(), "%"));
}
