#pragma once

// bw/modes/mode_manager.h — F24's pure logic (spec §6 D3/D4/D6, §7.1 matrix).
// Deterministic, platform-free, host-testable: Output step(const Input&)-style
// with an explicit time argument (01 §1 rule 5). No FreeRTOS/Arduino calls,
// no heap, no globals — the manager is a value the composition root owns.

#include <bw/commands.h>
#include <bw/hardware.h>
#include <bw/modes/mode_config.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::modes {

// Why a transition happened. Kept in W6 even though G1 ruling 1 (2026-10-18)
// defers mode-change events: it is ModeOutput's provenance field today and
// the future ModeChanged payload's reason byte (spec §6 D7). Feature-local on
// purpose — never a bw_core enum, no contract impact.
enum class ModeChangeReason : uint8_t {
    Command = 0,        // SetMode(...)
    AutoSchedule = 1,   // D4 schedule window crossing
    NapStart = 2,       // StartNap
    ResearchEnter = 3,  // EnterResearch
    ResearchExit = 4,   // ExitResearch
};

struct ModeInput {
    TimeMs t_ms;             // monotonic stamp for the produced output
    UnixSeconds unix_s;      // 0 = wall clock unknown → no auto switching (D4)
    bool has_command;        // one command per step; false = settle tick
    Command command;         // meaningful iff has_command
    BatteryReading battery;  // valid=false = unknown → Normal profile (D6)
};

struct ModeOutput {
    OperatingMode mode;           // mode after this step
    PowerProfile power;           // power profile after this step (orthogonal)
    bool changed;                 // a MODE transition happened this step; power-only is false
    OperatingMode previous_mode;  // mode at step start (== mode when !changed)
    ModeChangeReason reason;      // provenance of the (last) transition; meaningless when !changed
    bool wall_time_known;         // unix_s != 0 — auto scheduling is active
};

class ModeManager {
public:
    explicit ModeManager(const ModeConfig& config);

    // One pump tick, in priority order (spec §6 D3/D4):
    //   1. command (at most one) — rejected ⇒ Err::InvalidArg + unchanged state
    //   2. schedule crossing (D4, edge-triggered; suppressed outside Day/Night)
    //   3. power hysteresis (D6, independent axis)
    // The schedule and power axes still run after a rejected command, because
    // an illegal command must not stall the clock or the battery (§10).
    // previous_mode is ALWAYS the mode at step entry — in the rare tick where
    // a command and a crossing both transition, the output reports the net
    // effect and the final transition's reason.
    Result<ModeOutput> step(const ModeInput& in);

    OperatingMode mode() const { return mode_; }
    PowerProfile power() const { return power_; }

private:
    // D3 command semantics, one legal effect per command.
    void applyCommand(const Command& cmd, ModeOutput& out, Err& err);
    void transition(OperatingMode to, ModeChangeReason reason, ModeOutput& out);

    // D4: night window from wall hours (start>end wraps midnight).
    bool scheduledIsNight(UnixSeconds unix_s) const;

    // D6: hysteresis on the battery axis, independent of mode.
    void updatePower(const BatteryReading& battery);

    ModeConfig config_;
    OperatingMode mode_ = OperatingMode::Day;  // D8: boot is always Day
    PowerProfile power_ = PowerProfile::Normal;
    OperatingMode pre_research_mode_ = OperatingMode::Day;  // fallback Day (D3.4)
    // D4 crossing edge-detector: the schedule-derived mode of the last tick.
    // Starts at Day so a first tick already inside the night window counts as
    // a crossing (boot-time initial sync, spec §9 test 12).
    bool last_scheduled_night_ = false;
};

}  // namespace bw::modes
