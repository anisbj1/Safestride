#pragma once

// firmware/src/app/wiring/wire_modes.h — F24's wiring pair (convention:
// src/app/wiring/README.md; file name per F24 spec §2 Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).
// F24 creates NO task (integrator G1 ruling 2, 2026-10-18): the composition
// root calls modePumpOnce() from the existing ServicePump at
// bw::modes::kModePumpPeriodMs (2 s — the ServicePump's cadence, runtime.md
// §3.2). F24 emits NO events in W6 (G1 ruling 1): mode changes are
// provider-visible only.

#include <bw/commands.h>
#include <bw/mode_profile.h>
#include <bw/modes/mode_manager.h>
#include <bw/providers.h>  // bw::ICommandSink (02 §5)
#include <bw/result.h>

namespace app {
struct AppContext;
}

namespace bw {
class IBatteryMonitor;
}

namespace f24 {

// Constructs the ModeManager (function-local static: idempotent — a second
// call keeps the first instance —, no heap after startup) with the default
// ModeConfig. ctx.clock and `battery` may be nullptr: modePumpOnce() then
// answers Err::NotReady until they exist — an honest "not available"
// (spec §10), never a crash or a fabricated mode. The clock/battery pointers
// are re-bound on every call (last call wins); the manager instance is not.
void wireModesInit(app::AppContext& ctx, bw::IBatteryMonitor* battery);

// One pump tick (pure entry point, host-testable with a scripted clock and a
// fake battery — test_wire_modes_compile.cpp): reads clock + battery, drains
// at most bw::modes::kMaxCommandsPerPump queued commands (one step() per
// command, so every transition is reported once), runs one settle step
// (schedule + power), and republishes bw::ModeProfile EVERY tick (spec D2).
// Paced by the CALLER at bw::modes::kModePumpPeriodMs; does no pacing itself.
// Err::NotReady before wireModesInit() or when no clock is available.
bw::Result<void> modePumpOnce();

// Command seam (02 §5 ICommandSink): the app-level router pushes
// SetMode / StartNap / EnterResearch / ExitResearch here. Push on a full
// queue is refused (Err::Full semantics, CR-004) and counted by the queue's
// drops() — never blocking, never silently lost (spec §7.2 fan-out note).
bw::ICommandSink& modeCommandSink();

// Provider getter (spec §7.2): the newest published ModeProfile; false before
// the first pump. Read-only snapshot; consumers without a profile yet must
// fall back to their own defaults (bw/mode_profile.h).
bool modeProfile(bw::ModeProfile& out);

// The IModeProvider instance (02 §5, CR-027) for consumers that want the
// interface (integrator P4: F07 stamps StateSnapshot.mode/power from it).
// Null before wireModesInit().
const bw::IModeProvider* modeProvider();

}  // namespace f24
