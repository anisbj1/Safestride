// bw/modes/mode_manager.cpp — F24 pure logic: transition matrix (D3),
// crossing-based auto scheduler (D4), power hysteresis (D6) and the profile
// table (spec §8). No platform calls (01 §1 rule 5).

#include <bw/modes/mode_manager.h>

#include <cmath>

#include <bw/modes/mode_profile_table.h>

namespace bw::modes {

namespace {

// [DERIVED] OperatingMode has exactly five values (02 §4.5, append-only).
// This pins W6's legal argument range for SetMode (D3.6): a future append to
// the enum must revisit this constant deliberately, never silently.
constexpr uint32_t kModeCount = 5;

// Night window check over a 0..23 hour. start == end is an empty window
// (never night) — a misconfigured equal pair must not make the whole day
// "night".
bool hourInNightWindow(uint8_t hour, uint8_t start, uint8_t end) {
    if (start == end) {
        return false;
    }
    if (start < end) {
        return hour >= start && hour < end;
    }
    return hour >= start || hour < end;  // window wraps midnight (22..7)
}

}  // namespace

ModeProfileHints hintsFor(bw::OperatingMode mode, bw::PowerProfile power) {
    uint8_t brightness = kMaintBrightnessPct;
    bw::AlertLevel alert_cap = kMaintAlertCap;
    switch (mode) {
    case bw::OperatingMode::Day:
        brightness = kDayBrightnessPct;
        alert_cap = kDayAlertCap;
        break;
    case bw::OperatingMode::Night:
        brightness = kNightBrightnessPct;
        alert_cap = kNightAlertCap;
        break;
    case bw::OperatingMode::Nap:
        brightness = kNapBrightnessPct;
        alert_cap = kNapAlertCap;
        break;
    case bw::OperatingMode::Calibration:
    case bw::OperatingMode::Research:
        // Calibration and Research share the "maintenance" values (§8).
        brightness = kMaintBrightnessPct;
        alert_cap = kMaintAlertCap;
        break;
    }
    if (power == bw::PowerProfile::LowPower && brightness > kLowPowerBrightnessCapPct) {
        // LowPower caps brightness only; the alert cap is untouched (§8).
        brightness = kLowPowerBrightnessCapPct;
    }
    return {brightness, alert_cap};
}

ModeManager::ModeManager(const ModeConfig& config) : config_(config) {}

Result<ModeOutput> ModeManager::step(const ModeInput& in) {
    const OperatingMode start_mode = mode_;

    ModeOutput out{};
    out.mode = mode_;
    out.power = power_;
    out.changed = false;
    out.previous_mode = start_mode;
    out.reason = ModeChangeReason::Command;  // placeholder; only read when changed
    out.wall_time_known = in.unix_s != 0;

    // 1. Command (D3). A rejected command must not block the other axes.
    Err err = Err::Ok;
    if (in.has_command) {
        applyCommand(in.command, out, err);
    }

    // 2. Schedule crossing (D4): edge-triggered on the schedule itself, so an
    // explicit SetMode is never undone mid-window — only the next actual
    // boundary (plus/minus one tick) can move Day/Night. Auto never fires in
    // Nap/Calibration/Research; wall time unknown (unix_s == 0) means no auto
    // at all (00 §4: unknown time is first-class, never guessed).
    if (in.unix_s != 0) {
        const bool scheduled_night = scheduledIsNight(in.unix_s);
        if (scheduled_night != last_scheduled_night_) {
            // The edge detector ALWAYS updates, even outside Day/Night, so a
            // boundary crossed during Nap/Research does not fire retroactively
            // when the user returns to Day/Night (§9 test 10).
            last_scheduled_night_ = scheduled_night;
            const OperatingMode target =
                scheduled_night ? OperatingMode::Night : OperatingMode::Day;
            if ((mode_ == OperatingMode::Day || mode_ == OperatingMode::Night) && target != mode_) {
                transition(target, ModeChangeReason::AutoSchedule, out);
            }
        }
    }

    // 3. Power (D6): orthogonal axis, never a mode, never a `changed` flag.
    updatePower(in.battery);

    out.mode = mode_;
    out.power = power_;
    return {err, out};
}

void ModeManager::applyCommand(const Command& cmd, ModeOutput& out, Err& err) {
    switch (cmd.type) {
    case CommandType::SetMode: {
        if (cmd.arg0 >= kModeCount) {
            err = Err::InvalidArg;  // D3.6: out-of-enum, unchanged
            break;
        }
        const auto target = static_cast<OperatingMode>(cmd.arg0);
        if (target == mode_) {
            // (Also covers SetMode(Research) while IN Research — the matrix
            // diagonal cell is "no-op" and this is its only self-trigger.)
            break;  // D3.5: self-transition is an idempotent success
        }
        if (target == OperatingMode::Research) {
            // F24-A1 (audit 2026-10-18): Research is ENTERED only via
            // EnterResearch (§7.1 Research column, D3 "its own commands"),
            // because only EnterResearch stores pre_research_mode_ — a
            // SetMode(Research) entry would make ExitResearch restore a
            // stale pre-mode and silently lose the user's context.
            err = Err::InvalidArg;
            break;
        }
        if (mode_ == OperatingMode::Research) {
            err = Err::InvalidArg;  // D3.1: leaving Research needs ExitResearch
            break;
        }
        transition(target, ModeChangeReason::Command, out);
        break;
    }
    case CommandType::StartNap: {
        if (mode_ == OperatingMode::Research) {
            err = Err::InvalidArg;  // D3.2: same gate as SetMode
            break;
        }
        if (mode_ == OperatingMode::Nap) {
            break;  // D3.5: idempotent no-op
        }
        transition(OperatingMode::Nap, ModeChangeReason::NapStart, out);
        break;
    }
    case CommandType::EnterResearch: {
        if (mode_ == OperatingMode::Research) {
            err = Err::InvalidArg;  // D3.3: repeat enter rejected, no overwrite
            break;
        }
        pre_research_mode_ = mode_;  // D3.3: remember where to restore
        transition(OperatingMode::Research, ModeChangeReason::ResearchEnter, out);
        break;
    }
    case CommandType::ExitResearch: {
        if (mode_ != OperatingMode::Research) {
            err = Err::InvalidArg;  // D3.4: only Research can exit
            break;
        }
        // pre_research_mode_ is only ever set from a non-Research mode
        // (EnterResearch guard above), but stay defensive: Research must
        // never be its own fallback (spec §10: fallback Day).
        const OperatingMode back =
            pre_research_mode_ == OperatingMode::Research ? OperatingMode::Day : pre_research_mode_;
        transition(back, ModeChangeReason::ResearchExit, out);
        break;
    }
    default:
        // Not an F24 command: every command must have exactly one legal
        // effect (D3), and this one has none here — an honest rejection,
        // never a silent swallow.
        err = Err::InvalidArg;
        break;
    }
}

void ModeManager::transition(OperatingMode to, ModeChangeReason reason, ModeOutput& out) {
    mode_ = to;
    out.changed = true;
    out.reason = reason;  // last transition wins in a double-transition tick
}

bool ModeManager::scheduledIsNight(UnixSeconds unix_s) const {
    const uint32_t hour = (static_cast<uint32_t>(unix_s) / 3600u) % 24u;
    return hourInNightWindow(static_cast<uint8_t>(hour), config_.night_start_hour,
                             config_.night_end_hour);
}

void ModeManager::updatePower(const BatteryReading& battery) {
    if (!battery.valid) {
        power_ = bw::PowerProfile::Normal;  // §10: unknown battery ⇒ Normal
        return;
    }
    if (kLowPowerChargingOverrides && battery.charging_known && battery.charging) {
        power_ = bw::PowerProfile::Normal;  // mains available, D6
        return;
    }
    const float pct = battery.percent_estimate;
    if (!std::isfinite(pct)) {
        // A fabricated number must never drive the profile (00 §1 honesty):
        // non-finite percent is treated exactly like an invalid reading.
        power_ = bw::PowerProfile::Normal;
        return;
    }
    if (power_ == bw::PowerProfile::LowPower) {
        if (pct >= static_cast<float>(config_.low_power_exit_pct)) {
            power_ = bw::PowerProfile::Normal;  // D6 exit
        }
        // inside the band: hold (hysteresis, no flapping)
    } else if (pct <= static_cast<float>(config_.low_power_enter_pct)) {
        power_ = bw::PowerProfile::LowPower;  // D6 enter
    }
}

}  // namespace bw::modes
