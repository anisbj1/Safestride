#pragma once

// firmware/src/app/wiring/wire_ui.h — F12's wiring pair (convention:
// src/app/wiring/README.md). Construction, dependency injection and task
// creation only; all behavior lives in bw_ui, which the host suite drives
// with scripted timelines (01 §1 rule 3).
//
// Deferred integration hooks (§15 step 8 — documented here, NOT called by
// F12): the integrator routes F11's UI-bound commands to uiCommands(); menu
// MenuActions convert to Commands when a consumer exists; attachFaultReporter
// already takes ctx.faults and stays nullptr-safe until F23/F26 assign one.

#include <cstddef>
#include <cstdint>

#include <bw/providers.h>
#include <bw/ui/screen_manager.h>
#include <bw/ui/ui_config.h>

namespace app {
struct AppContext;
}

namespace f12 {

// [DERIVED] Exactly one frame per tick (§8): the task period equals the frame
// interval, so the ≤10 fps cap (spec §5) holds by construction.
constexpr uint32_t kUiTaskPeriodMs = static_cast<uint32_t>(bw::ui::kUiFrameIntervalMs);

// [EXPERIMENTAL] Stack bytes (§8): model frame + two snprintf line buffers +
// the render chain; re-measure with uxTaskGetStackHighWaterMark at DoD.
constexpr uint32_t kUiTaskStackBytes = 3072;

// [DERIVED] ≤3 per the W2 task ladder (§8): below InputTask's 3 (hard debounce
// timing) and F10's high-priority alert task — a late frame is cosmetic, a
// late button tick is not.
constexpr uint32_t kUiTaskPriority = 2;

// [DERIVED] runtime.md §3.2: UiTask on core 1 (mailbox reads are single-word
// anyway; one core avoids cross-core traffic).
constexpr uint32_t kUiTaskCore = 1;

// [DERIVED] UI commands are human-rate (a few/s worst case); 8 holds seconds
// of backlog. A full queue refuses the push (CR-004) and the queue counts the
// drop — the UI never blocks its producer (§10).
constexpr size_t kUiCommandQueueLen = 8;

// [DERIVED] Menu gestures are strictly slower than UI commands (one press per
// entry: select, then read the screen); 4 covers a burst of rapid presses with
// hours of headroom. Same refusal rule as above: a full queue drops the
// gesture, never blocks InputTask.
constexpr size_t kUiActionQueueLen = 4;

// F12's OWN command port (D4): `ctx.commands`'s documented producer/consumer
// pair is f11 → f10 (app.h), so F12 never pops it — whoever the integrator
// routes (F11's UI commands) pushes UI-bound Commands here instead. Single
// producer per SPSC rules; the consumer is f12::uiTask.
bw::ICommandSink* uiCommands();

// --- Menu closure (backlog 32, 2026-10-17) ----------------------------------
// D2-N leaves Menu·Big short and Menu·Small long as deliberate "no command"
// cells (F11 §6: no CommandType means "select" or "back", and inventing one
// needs a CR). The two gestures therefore travel as UI actions on their own
// port — never as Commands — and the UI-side state machine does the work.
enum class UiAction : uint8_t { Select, Back };

// Posts one menu gesture. Single producer = F11's InputTask (the same task that
// owns the uiCommands() pushes, so both ports keep their SPSC rule); consumer =
// f12::uiTask, which drains it into ScreenManager::menuSelect()/menuBack().
// A full queue refuses (CR-004) and the gesture is dropped, never retried —
// the press already happened (F11 D6).
void postUiAction(UiAction action);

// True while the menu is on screen. Published by uiTask each frame into a
// bw_core LatestValue; read by F11's InputTask to derive Menu as its
// InputContext (the F12 half of the UI-state→InputContext seam — bw_input
// still sees only a bool). False from boot, so the first press routes as Idle.
bool menuOpen();

// Staged menu semantic action (ScreenManager::menuSelect), drained once per
// gesture. Consumer = F11's InputTask, which converts it into a Command on
// ctx.commands (the documented f11 → f10 path, single producer preserved).
// Carries the ACTION, not the menu index, so F11 stays ignorant of menu
// contents (F11 §12).
bw::ui::MenuAction takeMenuAction();

// One pump of the input seam: drains uiCommands() and postUiAction() into the
// ScreenManager, forwards any staged MenuAction toward f11, and publishes
// menuOpen(). uiTask calls it once per frame; the host suite calls it directly
// (it lives outside the ARDUINO guard on purpose — 01 §1 rule 5).
void pumpInputs(bw::TimeMs now);

// W3 P4 (2026-10-13, CR-016): F14's prompt provider. uiTask polls it once per
// frame and flips the D6 integration point setPromptAvailable(bool) — phase
// Idle keeps the Prompt screen unreachable (W2's exact default). nullptr =
// today's behavior until the composition root attaches &f14::manager();
// consumers stay bw_core-only (01 §1 rule 1, providers.h contract).
void attachConfirmPromptProvider(bw::IConfirmPromptProvider* provider);

// Idempotence/order contract (§15 step 6): call once, in the "services" slot
// after f01::wire (the display/battery accessors must be valid) and f10's
// wire. Binds the AppContext mailboxes that exist today (state_snapshot,
// alert_status, ppg_features, clock, faults) and creates UiTask under
// ARDUINO; the F23 `health` mailbox is attached by SFINAE the moment the
// field exists — nothing to flip (D5).
void wire(app::AppContext& ctx);

#if defined(ARDUINO)
// UiTask body (runtime.md §3.2): drains uiCommands() into the model, steps
// the model, presents one frame (or nothing while timed out, D8), then paces
// itself with vTaskDelayUntil. `arg` is the AppContext.
void uiTask(void* arg);
#endif

}  // namespace f12
