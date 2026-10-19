#pragma once

// bw/input/input_router.h — the D2-N normative mapping table (F11 §6 D2).
// DoD §14: this table and the rows in test_input_router.cpp must be identical
// cell for cell; changing any cell needs developer confirmation (F11 §13).

#include <array>
#include <cstdint>

#include <bw/commands.h>
#include <bw/input/input_types.h>

namespace bw::input {

// Sentinel type for "no command" cells: route() returns false before ever
// reading it, so an invalid CommandType can never reach a Command. Named so a
// reader knows it is a placeholder, not a real command.
inline constexpr CommandType kNoCommandType = static_cast<CommandType>(0xFF);

struct MapCell {
    InputContextId context;
    GestureKey gesture;
    CommandType type;  // only meaningful when has_command (else kNoCommandType)
    uint32_t arg0;
    uint32_t arg1;
    bool has_command;
};

// All context x gesture cells — presence of the deliberate "no command" cells
// is what makes the table complete (and testable) by construction.
inline constexpr std::array<MapCell, kInputContextCount * kGestureKeyCount> kInputMap = {{
    // --- Idle -------------------------------------------------------------- Big short: menu entry
    // (§13 [ASSUMED]).
    {InputContextId::Idle, GestureKey::BigShort, CommandType::OpenMenu, 0, 0, true},
    {InputContextId::Idle, GestureKey::BigLong, kNoCommandType, 0, 0, false},
    {InputContextId::Idle, GestureKey::SmallShort, CommandType::NextScreen, kStepOnce, 0, true},
    {InputContextId::Idle, GestureKey::SmallLong, CommandType::MarkEvent, 0, 0, true},
    {InputContextId::Idle, GestureKey::Chord, CommandType::StartPairing, 0, 0, true},
    // --- AlertActive ------------------------------------------------------- Big short: ack (§13).
    {InputContextId::AlertActive, GestureKey::BigShort, CommandType::AcknowledgeAlert, 0, 0, true},
    {InputContextId::AlertActive, GestureKey::BigLong, CommandType::CancelAlert, 0, 0, true},
    {InputContextId::AlertActive, GestureKey::SmallShort, CommandType::NextScreen, kStepOnce, 0,
     true},
    {InputContextId::AlertActive, GestureKey::SmallLong, kNoCommandType, 0, 0, false},
    {InputContextId::AlertActive, GestureKey::Chord, CommandType::StartPairing, 0, 0, true},
    // --- ConfirmPrompt ----------------------------------------------------- Big short: CONFIRM
    // (§13).
    {InputContextId::ConfirmPrompt, GestureKey::BigShort, CommandType::ConfirmEvent, 0, 0, true},
    {InputContextId::ConfirmPrompt, GestureKey::BigLong, kNoCommandType, 0, 0, false},
    {InputContextId::ConfirmPrompt, GestureKey::SmallShort, CommandType::RejectEvent, 0, 0, true},
    {InputContextId::ConfirmPrompt, GestureKey::SmallLong, kNoCommandType, 0, 0, false},
    {InputContextId::ConfirmPrompt, GestureKey::Chord, CommandType::StartPairing, 0, 0, true},
    // --- Menu -------------------------------------------------------------- scroll rule via
    // kStepOnce (D7).
    {InputContextId::Menu, GestureKey::BigShort, kNoCommandType, 0, 0, false},
    {InputContextId::Menu, GestureKey::BigLong, kNoCommandType, 0, 0, false},
    {InputContextId::Menu, GestureKey::SmallShort, CommandType::NextScreen, kStepOnce, 0, true},
    {InputContextId::Menu, GestureKey::SmallLong, kNoCommandType, 0, 0, false},
    {InputContextId::Menu, GestureKey::Chord, CommandType::StartPairing, 0, 0, true},
}};

class InputRouter {
public:
    // Looks the (context, gesture) pair up in kInputMap. Returns true and fills
    // out with a button-sourced Command (t_ms = gesture time) for a mapped
    // cell; false for a "no command" cell or a missing cell (the completeness
    // test in the suite proves the table has all 20, so both paths agree).
    bool route(InputContextId context, const GestureRecord& record, Command& out) const;
};

}  // namespace bw::input
