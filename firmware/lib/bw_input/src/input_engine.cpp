#include <bw/input/input_engine.h>

namespace bw::input {

InputEngine::InputEngine() = default;

InputEngine::InputEngine(const InputEngineConfig& config)
    : debouncer_(config.debounce_ms), detector_(config.gesture) {}

StepOutput InputEngine::step(TimeMs t_ms, const InputContext& context, bool big_raw,
                             bool small_raw) {
    StepOutput out;
    const bool raw[2] = {big_raw, small_raw};
    if (context.id == InputContextId::AlertActive &&
        (big_raw || small_raw || pressed_[0] || pressed_[1])) {
        alert_gesture_ = true;
    }
    for (uint8_t i = 0; i < 2; ++i) {
        const ButtonId button = i == 0 ? ButtonId::Big : ButtonId::Small;
        const ButtonDebouncer::Level level = debouncer_.step(t_ms, button, raw[i]);
        pressed_[i] = level.pressed;
        const GestureDetector::Output dop =
            detector_.step(t_ms, button, level.pressed, level.changed);
        if (dop.stuck_edge) {
            out.stuck[buttonIndex(dop.stuck_button)] = true;
        }
        if (dop.has_event && out.count < kMaxGesturesPerStep) {
            RoutedGesture& item = out.items[out.count++];
            item.gesture = dop.event;
            item.alert_consumed = alert_gesture_ || context.id == InputContextId::AlertActive;
            item.has_button_event = toButtonEvent(dop.event, item.button_event);
            const InputContextId route_context =
                item.alert_consumed ? InputContextId::AlertActive : context.id;
            // During an alert both buttons are reserved. No navigation,
            // pairing or deferred menu action escapes, even on release.
            const bool alert_response =
                dop.event.key == GestureKey::BigShort || dop.event.key == GestureKey::BigLong;
            item.has_command = (!item.alert_consumed || alert_response) &&
                               router_.route(route_context, dop.event, item.command);
        }
    }
    if (!big_raw && !small_raw && !pressed_[0] && !pressed_[1]) {
        alert_gesture_ = false;
    }
    return out;
}

void InputEngine::reset() {
    debouncer_.reset();
    detector_.reset();
    alert_gesture_ = false;
    pressed_[0] = false;
    pressed_[1] = false;
}

}  // namespace bw::input
