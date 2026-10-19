// bw_ui — F12 §9 test_flush_failure: D8/§10 failure behaviour — a failed
// flush is retried next frame with no state loss, the consecutive-failure
// counter rises, exactly ONE DisplayFailed report happens per episode, and a
// recovery resumes normal frames.

#include <unity.h>

#include <bw/testkit/fake_canvas.h>
#include <bw/testkit/recording_fault_reporter.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "ui_fixtures.h"

using bw::testkit::FakeCanvas;
using bw::testkit::RecordingFaultReporter;
using bw_ui_test::snapshot;

namespace {

// [EXPERIMENTAL] Threshold 3 keeps the episode timeline at 3 ticks; the
// production default is kFlushFailFaultThreshold (20), the behavior is
// identical.
bw::ui::UiConfig lowThresholdConfig() {
    bw::ui::UiConfig config = bw::ui::defaultUiConfig();
    config.flush_fail_threshold = 3;
    return config;
}

bw::ui::UiFrame healthyFrame(bw::ui::UiModel& model, bw::ui::UiInputs& inputs,
                             bw::LatestValue<bw::StateSnapshot>& state) {
    state.set(snapshot(bw::AlertnessState::Awake, bw::WearState::Worn));
    inputs.state = &state;
    return model.step(inputs, 0);
}

}  // namespace

void test_flush_failure_counter_rises_and_retries_every_frame(void) {
    bw::ui::UiModel model{lowThresholdConfig()};
    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    const bw::ui::UiFrame frame = healthyFrame(model, inputs, state);

    FakeCanvas canvas;
    canvas.setFlushResult(bw::Err::BusError);
    bw::ui::ScreenRenderer renderer{lowThresholdConfig()};

    for (int i = 0; i < 5; ++i) {
        renderer.render(frame, canvas, static_cast<bw::TimeMs>(i * 100));
    }
    // No crash, no state loss: every tick redrew and retried the flush.
    TEST_ASSERT_EQUAL_UINT(5, static_cast<unsigned>(canvas.flushCount()));
    TEST_ASSERT_EQUAL_UINT(5, static_cast<unsigned>(renderer.consecutiveFlushFailures()));
    TEST_ASSERT_TRUE(renderer.powerOn());
}

void test_flush_failure_reports_exactly_once_per_episode(void) {
    bw::ui::UiModel model{lowThresholdConfig()};
    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    const bw::ui::UiFrame frame = healthyFrame(model, inputs, state);

    FakeCanvas canvas;
    canvas.setFlushResult(bw::Err::BusError);
    bw::ui::ScreenRenderer renderer{lowThresholdConfig()};
    RecordingFaultReporter<8> faults;
    renderer.attachFaultReporter(&faults);

    for (int i = 0; i < 10; ++i) {  // far past the threshold of 3
        renderer.render(frame, canvas, static_cast<bw::TimeMs>(i * 100));
    }
    TEST_ASSERT_EQUAL_UINT(1, static_cast<unsigned>(faults.countOf(bw::FaultCode::DisplayFailed)));
    TEST_ASSERT_EQUAL_UINT(1, static_cast<unsigned>(faults.count()));
}

void test_flush_failure_recovery_resumes_normal_frames(void) {
    bw::ui::UiModel model{lowThresholdConfig()};
    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    const bw::ui::UiFrame frame = healthyFrame(model, inputs, state);

    FakeCanvas canvas;
    canvas.setFlushResult(bw::Err::BusError);
    bw::ui::ScreenRenderer renderer{lowThresholdConfig()};
    RecordingFaultReporter<8> faults;
    renderer.attachFaultReporter(&faults);
    for (int i = 0; i < 3; ++i) {  // episode 1 reaches the threshold of 3
        renderer.render(frame, canvas, static_cast<bw::TimeMs>(i * 100));
    }
    TEST_ASSERT_EQUAL_UINT(3, static_cast<unsigned>(renderer.consecutiveFlushFailures()));
    TEST_ASSERT_EQUAL_UINT(1, static_cast<unsigned>(faults.countOf(bw::FaultCode::DisplayFailed)));

    canvas.setFlushResult(bw::Err::Ok);
    renderer.render(frame, canvas, 300);
    TEST_ASSERT_EQUAL_UINT(0, static_cast<unsigned>(renderer.consecutiveFlushFailures()));

    // A NEW failure episode may report once more (the counter was reset).
    canvas.setFlushResult(bw::Err::BusError);
    for (int i = 0; i < 5; ++i) {
        renderer.render(frame, canvas, static_cast<bw::TimeMs>(400 + i * 100));
    }
    TEST_ASSERT_EQUAL_UINT(2, static_cast<unsigned>(faults.countOf(bw::FaultCode::DisplayFailed)));
}

void test_flush_failure_without_reporter_is_safe(void) {
    bw::ui::UiModel model{lowThresholdConfig()};
    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    const bw::ui::UiFrame frame = healthyFrame(model, inputs, state);

    FakeCanvas canvas;
    canvas.setFlushResult(bw::Err::BusError);
    bw::ui::ScreenRenderer renderer{lowThresholdConfig()};
    for (int i = 0; i < 10; ++i) {  // §10: nullptr reporter = count only, never crash
        renderer.render(frame, canvas, static_cast<bw::TimeMs>(i * 100));
    }
    TEST_ASSERT_EQUAL_UINT(10, static_cast<unsigned>(renderer.consecutiveFlushFailures()));
}

void test_flush_failure_none_while_powered_off(void) {
    bw::ui::UiConfig config = lowThresholdConfig();
    config.display_timeout_ms = 500;
    bw::ui::UiModel model{config};
    bw::ui::UiInputs inputs;
    bw::LatestValue<bw::StateSnapshot> state;
    (void)healthyFrame(model, inputs, state);

    FakeCanvas canvas;
    canvas.setFlushResult(bw::Err::BusError);
    bw::ui::ScreenRenderer renderer{config};
    renderer.attachFaultReporter(nullptr);

    const bw::ui::UiFrame off = model.step(inputs, 600);  // timed out
    TEST_ASSERT_FALSE(off.power);
    renderer.render(off, canvas, 600);
    TEST_ASSERT_EQUAL_UINT(0, static_cast<unsigned>(canvas.flushCount()));  // bus quiet
    TEST_ASSERT_EQUAL_UINT(0, static_cast<unsigned>(renderer.consecutiveFlushFailures()));
}
