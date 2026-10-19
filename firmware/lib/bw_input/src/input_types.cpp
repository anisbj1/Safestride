#include <bw/input/input_types.h>

namespace bw::input {

InputContextId deriveContext(const AlertStatus* alert) {
    return deriveContext(alert, false);
}

InputContextId deriveContext(const AlertStatus* alert, bool menu_open) {
    // AlertActive outranks Menu: the menu may be open on the panel when an
    // episode starts, and the user must still be able to acknowledge it
    // (never let a feature block immediate alerting).
    if (alert != nullptr && alert->level != AlertLevel::Normal) {
        return InputContextId::AlertActive;
    }
    return menu_open ? InputContextId::Menu : InputContextId::Idle;
}

bool toButtonEvent(const GestureRecord& record, ButtonEvent& out) {
    switch (record.key) {
    case GestureKey::BigShort:
        out = ButtonEvent{record.t_ms, ButtonId::Big, ButtonGesture::ShortPress};
        return true;
    case GestureKey::BigLong:
        out = ButtonEvent{record.t_ms, ButtonId::Big, ButtonGesture::LongPress};
        return true;
    case GestureKey::SmallShort:
        out = ButtonEvent{record.t_ms, ButtonId::Small, ButtonGesture::ShortPress};
        return true;
    case GestureKey::SmallLong:
        out = ButtonEvent{record.t_ms, ButtonId::Small, ButtonGesture::LongPress};
        return true;
    case GestureKey::Chord:
        break;  // not representable in bw::ButtonGesture (see header)
    }
    return false;
}

}  // namespace bw::input
