#pragma once

// bw/input/input_types.h — the vocabulary F11 routes with: contexts (D2-N),
// gesture keys, and the two pure helpers that touch bw_core value types
// (deriveContext, toButtonEvent). bw_core only; no GPIO, no platform calls.

#include <cstdint>

#include <bw/alerts.h>
#include <bw/input.h>
#include <bw/input/input_constants.h>

namespace bw::input {

// D2-N contexts. Idle/AlertActive are derived from AlertStatus in wave 1 (D4);
// ConfirmPrompt/Menu are defined and tabled now, their sources (F12/F14) arrive
// later as an additive AppContext field.
enum class InputContextId : uint8_t { Idle, AlertActive, ConfirmPrompt, Menu };
constexpr uint8_t kInputContextCount = 4;

// D2-N gesture keys. Kept separate from bw::ButtonGesture: that contract enum
// has no Chord value (appending one is a contract change, 02 §6.6) and no
// consumer needs one today — the chord travels as a GestureRecord plus its
// StartPairing command, and toButtonEvent() reports it as "not representable".
enum class GestureKey : uint8_t { BigShort, BigLong, SmallShort, SmallLong, Chord };
constexpr uint8_t kGestureKeyCount = 5;

struct InputContext {
    InputContextId id = InputContextId::Idle;
};

// Stable index into the per-button arrays (ButtonId is Big=0, Small=1 today;
// this helper keeps every array access in one place).
constexpr uint8_t buttonIndex(ButtonId button) {
    return button == ButtonId::Big ? 0u : 1u;
}

// D4: NULL (mailbox empty) or level Normal -> Idle; any raised level ->
// AlertActive. Alert staleness policy belongs to F10, not here.
InputContextId deriveContext(const AlertStatus* alert);

// Menu closure (backlog 32). The F12 menu is UI state that an AlertStatus
// cannot express, so the wiring carries it in as a plain bool (F11 §6: Menu's
// source is "F12, arriving later"; this is that seam, kept OUTSIDE the router
// so bw_input still has no UI knowledge). AlertActive always wins over Menu —
// a raised alert is never blocked by UI state — and Menu is offered only while
// the alert side is quiet. The single-argument overload is exactly this
// function with menu_open = false, so wave-1 behaviour is unchanged.
InputContextId deriveContext(const AlertStatus* alert, bool menu_open);

struct GestureRecord {
    TimeMs t_ms = 0;
    GestureKey key = GestureKey::BigShort;
};

// F11 §3 output side: single-button gestures map 1:1 onto bw::ButtonEvent.
// Chord returns false (out untouched): bw::ButtonGesture has no Chord value and
// no consumer needs it — a future consumer raises a CR to append one.
bool toButtonEvent(const GestureRecord& record, ButtonEvent& out);

}  // namespace bw::input
