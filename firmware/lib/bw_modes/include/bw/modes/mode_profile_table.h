#pragma once

// bw/modes/mode_profile_table.h — F24 spec §8 / §7.2: the (mode, power) →
// {ui_brightness_pct, alert_max_level} lookup behind bw::ModeProfile
// (CR-027). These are HINTS, never commands (bw/mode_profile.h): the
// consuming subsystem applies what fits it and falls back to its own
// defaults when no profile exists.

#include <cstdint>

#include <bw/alerts.h>
#include <bw/state.h>

namespace bw::modes {

// [EXPERIMENTAL] All values are starting points (01 header note); they live
// here (not inline in logic) so tuning is a one-file change (spec §8 table).
// Why 100: full brightness in day — maximum readability, no power concern
// flagged yet.
constexpr uint8_t kDayBrightnessPct = 100;
// [EXPERIMENTAL] Why 40: readable in a dark room but non-blinding at night.
constexpr uint8_t kNightBrightnessPct = 40;
// [EXPERIMENTAL] Why 30: nap means the wrist rests near the face; the user is
// trying to sleep, so dimmer than night.
constexpr uint8_t kNapBrightnessPct = 30;
// [EXPERIMENTAL] Why 80: Calibration/Research must stay readable (the user is
// actively working with the device) without the full day drain.
constexpr uint8_t kMaintBrightnessPct = 80;
// [EXPERIMENTAL] Why 50: LowPower caps brightness; alert level is deliberately
// NOT reduced (safety-relevant feedback stays intact, spec §8).
constexpr uint8_t kLowPowerBrightnessCapPct = 50;

// [EXPERIMENTAL] Alert-ladder cap per mode (spec §8; ladder is F10's).
// Why Persistent in day: daytime is the socially active context where a
// missed alert costs most, so the full ladder is allowed.
constexpr bw::AlertLevel kDayAlertCap = bw::AlertLevel::Persistent;
// [EXPERIMENTAL] Why Active at night: quieter hours — still escalates above
// warning, but never the hard Persistent repeat.
constexpr bw::AlertLevel kNightAlertCap = bw::AlertLevel::Active;
// [EXPERIMENTAL] Why Active for nap: same reasoning as night (user asleep).
constexpr bw::AlertLevel kNapAlertCap = bw::AlertLevel::Active;
// [EXPERIMENTAL] Why Warning for Calibration/Research: the user is actively
// working with or debugging the device; loud repeated alerts would be noise,
// so the ladder is capped at warning.
constexpr bw::AlertLevel kMaintAlertCap = bw::AlertLevel::Warning;

struct ModeProfileHints {
    uint8_t ui_brightness_pct;
    bw::AlertLevel alert_max_level;
};

// Pure lookup, O(1): mode picks the base values (§8 table); power is applied
// last and only to brightness — Night+LowPower is a legal combination (00 §6,
// pipeline_and_state §6) and LowPower dims the screen but never downgrades
// alert intensity (§8 kLowPowerBrightnessCapPct why).
ModeProfileHints hintsFor(bw::OperatingMode mode, bw::PowerProfile power);

}  // namespace bw::modes
