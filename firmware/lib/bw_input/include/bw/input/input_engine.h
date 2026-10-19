#pragma once

// bw/input/input_engine.h — the one object InputTask drives (F11 §7):
// debouncer -> gesture detector -> router in a single pure step(). Reads raw
// levels and an InputContext, returns routed gestures and stuck edges; it owns
// no interfaces, so the host suite can drive it with scripted timelines.

#include <cstdint>

#include <bw/input/button_debouncer.h>
#include <bw/input/gesture_detector.h>
#include <bw/input/input_router.h>
#include <bw/input/input_types.h>
#include <bw/types.h>

namespace bw::input {

// Both buttons can finish within one 10 ms tick (two releases, or a release
// plus a stuck edge on the other button), never more.
constexpr uint8_t kMaxGesturesPerStep = 2;

struct InputEngineConfig {
    TimeMs debounce_ms = kDebounceMs;
    GestureDetectorConfig gesture{};
};

struct RoutedGesture {
    bool alert_consumed = false;  // includes release after the alert disappeared
    GestureRecord gesture{};
    ButtonEvent button_event{};  // valid when has_button_event
    bool has_button_event = false;
    Command command{};  // valid when has_command
    bool has_command = false;
};

struct StepOutput {
    uint8_t count = 0;  // routed gestures emitted this step (0..2)
    RoutedGesture items[kMaxGesturesPerStep]{};
    bool stuck[2] = {false, false};  // ButtonStuck edge per buttonIndex()
};

class InputEngine {
public:
    InputEngine();
    explicit InputEngine(const InputEngineConfig& config);

    // One 100 Hz tick (the period is the caller's; any spacing works). The
    // engine never reads a clock — t_ms comes from the caller (conventions).
    StepOutput step(TimeMs t_ms, const InputContext& context, bool big_raw, bool small_raw);

    // Drops all accumulated state (tests; a fresh boot starts the same way).
    void reset();

private:
    ButtonDebouncer debouncer_;
    GestureDetector detector_;
    InputRouter router_;
    bool alert_gesture_ = false;
    bool pressed_[2] = {false, false};
};

}  // namespace bw::input
