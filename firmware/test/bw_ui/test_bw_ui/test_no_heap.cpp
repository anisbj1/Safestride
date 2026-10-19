// bw_ui — F12 §9 test_no_heap: the steady-state frame path (UiModel::step +
// ScreenRenderer::render) must allocate 0 times. Same allocation-hook pattern
// as bw_ml/test_no_heap.cpp (bw_testkit has no shared hook, §9).

#include <cstddef>
#include <cstdlib>
#include <new>

#include <unity.h>

#include <bw/testkit/fake_canvas.h>
#include <bw/ui/screens.h>
#include <bw/ui/text_layout.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

namespace {

std::size_t g_allocation_count = 0;

// [EXPERIMENTAL] Frame iterations per §9's "N x step + render in steady
// state": 200 covers timeout edges (15 s at 10 fps = 150 frames) plus margin.
constexpr int kFrameIterations = 200;

}  // namespace

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    ++g_allocation_count;
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void test_no_heap_no_allocation_in_steady_state_frames(void) {
    using bw::testkit::FakeCanvas;

    bw::ui::UiModel model{};
    bw::ui::ScreenRenderer renderer{};
    FakeCanvas canvas;

    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    bw::LatestValue<bw::AlertStatus> alert_mailbox;
    bw::LatestValue<bw::PpgFeatures> ppg;
    bw::LatestValue<bw::HealthSnapshot> health;
    bw_ui_test::FakeBattery battery = bw_ui_test::FakeBattery::atPercent(87.0f);
    inputs.state = &state;
    inputs.alert = &alert_mailbox;
    inputs.ppg = &ppg;
    inputs.health = &health;
    inputs.battery = &battery;

    state.set(bw_ui_test::snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    ppg.set(bw_ui_test::ppgHr(72.0f, 100));
    health.set(bw_ui_test::health(bw::DataQuality::Good, bw::DataQuality::Good));

    // Warm-up frame: any lazy/static initialization happens BEFORE the
    // snapshot (same pattern as bw_ml's test).
    bw::TimeMs now = 0;
    renderer.render(model.step(inputs, now), canvas, now);
    (void)model.takeAction();  // staged-action drain must not allocate either

    const std::size_t before = g_allocation_count;
    for (int i = 1; i <= kFrameIterations; ++i) {
        now = static_cast<bw::TimeMs>(i) * bw::ui::kUiFrameIntervalMs;
        if (i % 50 == 0) {
            model.handleCommand(bw_ui_test::command(bw::CommandType::NextScreen), now);
        }
        renderer.render(model.step(inputs, now), canvas, now);
        (void)model.takeAction();
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}

void test_budget_ui_state_size(void) {
    // §11 budget: RAM < 2 KB for the screen state (UiModel frame +
    // ScreenManager + line buffers; the framebuffer lives in F01). The line
    // buffers are render-path stack; the widest render path holds a Medium
    // and a Small/offset buffer at once (audit F-2 font-sized buffers).
    const size_t line_buffers = sizeof(bw::ui::MediumLineBuffer) + sizeof(bw::ui::SmallLineBuffer);
    TEST_ASSERT_LESS_THAN_UINT(
        2048, static_cast<unsigned>(sizeof(bw::ui::UiFrame) + sizeof(bw::ui::UiModel) +
                                    sizeof(bw::ui::ScreenManager) + line_buffers));
    // Generous individual caps so a field addition that doubles the frame
    // size fails here rather than at the bench.
    TEST_ASSERT_LESS_THAN_UINT(128, static_cast<unsigned>(sizeof(bw::ui::UiFrame)));
    TEST_ASSERT_LESS_THAN_UINT(512, static_cast<unsigned>(sizeof(bw::ui::UiModel)));
    TEST_ASSERT_LESS_THAN_UINT(64, static_cast<unsigned>(sizeof(bw::ui::ScreenManager)));
}
