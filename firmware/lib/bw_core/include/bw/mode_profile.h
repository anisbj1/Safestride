#pragma once

// bw/mode_profile.h — CR-027 (accepted 2026-10-18, W6 G1): the operating-mode
// profile other features read. F24 produces it; F01 (sampling), F10 (alert
// intensity) and F12 (brightness) consume it when they next touch their code.
//
// It lives in bw_core because consumers may depend on bw_core only (01 §1 rule 1)
// and must never include F24's headers. In-memory only, never persisted or
// transmitted — no wire/schema impact beyond the contract pin below.

#include <bw/alerts.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw {

// Read-only snapshot of "how the device should behave right now". Every field is
// a HINT, never a command: the owning subsystem decides whether a hint applies
// (00 §1 honesty rule — a profile is not a guarantee, and a consumer that has no
// profile must fall back to its own defaults, never to a guessed mode).
struct ModeProfile {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs t_ms;                 // when produced (monotonic; 0 = never produced)
    OperatingMode mode;          // bw/state.h
    PowerProfile power;          // bw/state.h — orthogonal to mode (00 §6)
    uint8_t ui_brightness_pct;   // 0..100 hint for F12
    AlertLevel alert_max_level;  // cap for the F10 ladder (02 §4.6)
    bool wall_time_known;        // true = auto day/night scheduling is active
};

// F24 implements; false = no profile published yet.
class IModeProvider {
public:
    virtual bool latest(ModeProfile&) const = 0;
};

}  // namespace bw
