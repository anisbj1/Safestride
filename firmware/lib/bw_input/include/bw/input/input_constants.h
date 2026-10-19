#pragma once

// bw/input/input_constants.h — F11 §8: every value the input pipeline uses,
// named, tagged and justified. Single source of truth; the spec's Constants
// table and this file must stay identical (H9 spec<->code drift).

#include <cstdint>

#include <bw/types.h>

namespace bw::input {

// [EXPERIMENTAL] Typical tactile-switch bounce is 5-20 ms (F11 §8); 20 ms
// rejects a worst-case bounce while adding only two 100 Hz samples of lag.
constexpr TimeMs kDebounceMs = 20;

// [USER] Long enough (0.8 s) that a sleeve or pocket press never reads as a
// long press, short enough for a drowsy user to hold deliberately (F11 §8).
constexpr TimeMs kLongPressMs = 800;

// [EXPERIMENTAL] Deliberate two-button hold; only the rare chord actions
// (pairing) wait this long (F11 §8).
constexpr TimeMs kChordHoldMs = 3000;

// [DERIVED] bw/fault.h defines FaultCode::ButtonStuck as "pressed > 10 s"
// (F11 §10), so the code and the threshold can never drift apart; 10 s also
// outlasts any realistic sleeve press while freeing a jammed button quickly.
constexpr TimeMs kButtonStuckMs = 10000;

// [EXPERIMENTAL] After a chord fires, the two releases land milliseconds apart;
// a 0.5 s suppression window keeps that rebound from registering as a fresh
// press that immediately acts on whatever the chord just opened (F11 §8).
constexpr TimeMs kChordReleaseMs = 500;

// [DERIVED] arg0 of CommandType::NextScreen = "advance exactly one step";
// F12's screen model resolves one step to next-option-else-next-screen, so F11
// never needs option counts (F11 D7). 1 leaves 0 free for a future
// absolute/no-op meaning.
constexpr uint32_t kStepOnce = 1;

// [ASSUMED] Command.source byte for button-produced commands, from the
// "Button | Ble | Internal" comment order in bw/commands.h (no enum exists in
// bw_core today); pinned by test_contract.cpp until a CR pins the enum.
constexpr uint8_t kCommandSourceButton = 0;

}  // namespace bw::input
