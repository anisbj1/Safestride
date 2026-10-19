// firmware/src/app/wiring/wire_modes.cpp — F24's construction only.

#include "wire_modes.h"

#include "../app.h"  // AppContext: ctx.clock is read here, at construction

#include <bw/containers/latest_value.h>
#include <bw/containers/spsc_queue.h>
#include <bw/hardware.h>
#include <bw/modes/mode_config.h>
#include <bw/modes/mode_profile_table.h>

namespace f24 {

namespace {

// File-static wiring state (same pattern as F16's wire_history): pointers are
// set by wireModesInit(), the accessors stay honest (nullptr / false /
// Err::NotReady) until then. Single-owner house rule; the LatestValue copy is
// tear-free anyway (bw/containers/latest_value.h).
bw::modes::ModeManager* s_manager = nullptr;
const bw::IClock* s_clock = nullptr;       // not owned; the composition root owns it
bw::IBatteryMonitor* s_battery = nullptr;  // not owned; may stay null (honest unknown)

// F24-owned mailbox + command queue (spec §7.2, D2/D3 fan-out seam). They
// live here, not in AppContext, because app.h is integrator-owned; at P4 the
// integrator may lift them there without touching bw_modes. The mailbox is
// republished every tick (D2), so a late reader still converges.
bw::LatestValue<bw::ModeProfile> s_profile;
bw::SpscQueue<bw::Command, bw::modes::kModeCommandQueueDepth> s_commands;

bw::ModeProfile makeProfile(const bw::modes::ModeOutput& out, bw::TimeMs t_ms) {
    const bw::modes::ModeProfileHints hints = bw::modes::hintsFor(out.mode, out.power);
    bw::ModeProfile p{};
    p.t_ms = t_ms;
    p.mode = out.mode;
    p.power = out.power;
    p.ui_brightness_pct = hints.ui_brightness_pct;
    p.alert_max_level = hints.alert_max_level;
    p.wall_time_known = out.wall_time_known;
    return p;
}

// The two contract seams (02 §5): one writer-side sink for the command
// router, one read-only provider for mode/power consumers.
class ModeCommandSink final : public bw::ICommandSink {
public:
    bool push(const bw::Command& c) override {
        return s_commands.push(c) == bw::Err::Ok;  // full ⇒ refused + counted
    }
};

class ModeProvider final : public bw::IModeProvider {
public:
    bool latest(bw::ModeProfile& out) const override { return s_profile.get(out); }
};

ModeCommandSink s_sink;
ModeProvider s_provider;

}  // namespace

void wireModesInit(app::AppContext& ctx, bw::IBatteryMonitor* battery) {
    // Function-local static: constructed on the first call only, so a second
    // wireModesInit() keeps the first instance (no double-init, no heap after
    // startup — 00 §7.2); only the dependency pointers re-bind.
    static bw::modes::ModeManager instance{bw::modes::ModeConfig{}};
    s_manager = &instance;
    s_clock = ctx.clock;
    s_battery = battery;
}

bw::Result<void> modePumpOnce() {
    if (s_manager == nullptr || s_clock == nullptr) {
        return {bw::Err::NotReady};  // not wired / no clock yet (honest)
    }
    const bw::TimeMs t_ms = s_clock->nowMs();
    const bw::UnixSeconds unix_s = s_clock->unixSeconds();

    bw::BatteryReading battery{};
    battery.valid = false;  // no monitor wired is an honest "unknown" (§10)
    if (s_battery != nullptr) {
        battery = s_battery->read();
    }

    // Drain: one step() per command so each transition gets its own output
    // (at most kMaxCommandsPerPump per tick — bounded work, D9). A rejected
    // command's Result is intentionally folded: state-unchanged is its own
    // report to the (button/BLE) caller, and the settle step below publishes
    // the authoritative profile either way.
    bw::Command cmd;
    for (uint8_t n = 0; n < bw::modes::kMaxCommandsPerPump && s_commands.pop(cmd); ++n) {
        bw::modes::ModeInput in{};
        in.t_ms = t_ms;
        in.unix_s = unix_s;
        in.has_command = true;
        in.command = cmd;
        in.battery = battery;
        (void)s_manager->step(in);
    }

    // Settle tick: schedule crossing + power, no command.
    bw::modes::ModeInput in{};
    in.t_ms = t_ms;
    in.unix_s = unix_s;
    in.has_command = false;
    in.battery = battery;
    const bw::Result<bw::modes::ModeOutput> out = s_manager->step(in);

    s_profile.set(makeProfile(out.value, t_ms));  // every tick (D2)
    return {};
}

bw::ICommandSink& modeCommandSink() {
    return s_sink;
}

bool modeProfile(bw::ModeProfile& out) {
    return s_profile.get(out);
}

const bw::IModeProvider* modeProvider() {
    return s_manager == nullptr ? nullptr : &s_provider;
}

}  // namespace f24
