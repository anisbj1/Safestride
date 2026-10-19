#pragma once

// bw/commands.h — docs/02_INTERFACES.md §4.6 (commands: buttons, BLE, internal).
// Command is a compact fixed-size record also transmitted in the BLE Command
// characteristic (02 §6.2); its layout may not grow without a CR.

#include <cstdint>

#include <bw/types.h>

namespace bw {

enum class CommandType : uint8_t {
    AcknowledgeAlert,
    CancelAlert,
    NextScreen,
    OpenMenu,
    MarkEvent,
    ConfirmEvent,
    RejectEvent,
    StartNap,
    CancelNap,
    SetMode,
    SetSetting,
    SetTime,
    StartPairing,
    EnterResearch,
    ExitResearch,
    RequestSync,
    // … append only, never renumber (02 §6.6)
};

struct Command {
    CommandType type;
    TimeMs t_ms;
    uint8_t source;  // Button | Ble | Internal (02 §4.6)
    uint32_t arg0;
    uint32_t arg1;
};

// Per-Command argument semantics (02 §4.6; CR-003 accepted 2026-09-30 documents the
// SetSetting case):
//
//  SetSetting: arg0 = static_cast<uint32_t>(SettingKey); arg1 = the value's raw
//  32 bits — Bool: 0 / 1 · Int: static_cast<uint32_t>(int32 bits) · Float: IEEE-754
//  bit pattern. A receiver resolves the key's SettingType from ISettings::list()
//  (bw/providers.h) and decodes arg1 accordingly; an unrecognised key is dropped
//  (Err::NotFound), never guessed at.

}  // namespace bw
