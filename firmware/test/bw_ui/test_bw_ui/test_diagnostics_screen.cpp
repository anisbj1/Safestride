// bw_ui — F12 §9 test_diagnostics_screen: golden renders from a hand-built
// HealthSnapshot (bw_core contract type — F23 itself is not depended on) and
// the W2 default where no health mailbox exists yet (nullptr ⇒ "n/a").

#include <cstring>

#include <unity.h>

#include <bw/containers/latest_value.h>
#include <bw/testkit/fake_canvas.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw_ui_test::health;
using bw_ui_test::loadGolden;

namespace {

void renderDiag(bw::ui::UiModel& model, const bw::ui::UiInputs& inputs, bw::TimeMs now,
                FakeCanvas& canvas) {
    model.handleCommand(bw_ui_test::command(bw::CommandType::NextScreen), now);  // -> Diagnostics
    bw::ui::ScreenRenderer renderer;
    renderer.render(model.step(inputs, now), canvas, now);
}

}  // namespace

void test_diagnostics_screen_golden_degraded_sources(void) {
    bw::LatestValue<bw::HealthSnapshot> mailbox;
    mailbox.set(health(bw::DataQuality::Good, bw::DataQuality::Degraded));

    bw::ui::UiInputs inputs;
    inputs.health = &mailbox;
    bw::ui::UiModel model;
    FakeCanvas canvas;
    renderDiag(model, inputs, 1000, canvas);

    char golden[2048];
    loadGolden("diag_degraded", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "ppg:degraded"));  // §9 row
    // "mflt" = monitor-scoped faults (backlog 24): this is bw_diag's own
    // ledger, NOT the app-level ctx.faults registry. Asserting the full
    // "mflt:5" rather than a bare "flt:5" so a silent reversion to the old
    // ambiguous label fails here instead of passing as a substring.
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "mflt:5"));       // monitor fault_bits
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "heap 123456"));  // min_free_heap
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "ble:no"));       // ble_connected false
}

void test_diagnostics_screen_golden_no_health_source(void) {
    bw::ui::UiInputs inputs;  // health == nullptr until F23 wiring lands (D5)
    bw::ui::UiModel model;
    FakeCanvas canvas;
    renderDiag(model, inputs, 0, canvas);

    char golden[2048];
    loadGolden("diag_na", golden, sizeof(golden));
    TEST_ASSERT_EQUAL_STRING(golden, canvas.log());
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "n/a"));
}

void test_diagnostics_screen_empty_health_mailbox_is_na(void) {
    // Mailbox wired but nothing published yet — also an honest "n/a".
    bw::LatestValue<bw::HealthSnapshot> mailbox;
    bw::ui::UiInputs inputs;
    inputs.health = &mailbox;
    bw::ui::UiModel model;
    FakeCanvas canvas;
    renderDiag(model, inputs, 0, canvas);
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "imu:n/a"));
}

void test_diagnostics_screen_all_sources_insufficient_shown(void) {
    bw::LatestValue<bw::HealthSnapshot> mailbox;
    mailbox.set(health(bw::DataQuality::Insufficient, bw::DataQuality::Insufficient));

    bw::ui::UiInputs inputs;
    inputs.health = &mailbox;
    bw::ui::UiModel model;
    FakeCanvas canvas;
    renderDiag(model, inputs, 1000, canvas);
    // Audit observation: the token tracks the enum vocabulary ("insufficient",
    // not "low"); the combined line then truncates at the 21-char Small cap —
    // honestly, per D7, with the first source fully readable.
    TEST_ASSERT_NOT_NULL(std::strstr(canvas.log(), "imu:insufficient"));
    TEST_ASSERT_NULL(std::strstr(canvas.log(), "low"));
}
