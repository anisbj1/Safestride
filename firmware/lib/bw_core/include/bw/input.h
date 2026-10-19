#pragma once

// bw/input.h — docs/02_INTERFACES.md §4.6 (button input side).
// ButtonId sits here (not in bw/hardware.h) because both ButtonEvent and the
// IButtonInput interface refer to it; bw/hardware.h includes this header for it.

#include <bw/types.h>

namespace bw {

enum class ButtonId : uint8_t { Big, Small };

enum class ButtonGesture : uint8_t { ShortPress, LongPress, DoublePress };

struct ButtonEvent {
    TimeMs t_ms;
    ButtonId button;
    ButtonGesture gesture;
};

}  // namespace bw
