#include "wire_input.h"
#include "task_health.h"

#include <type_traits>
#include <utility>

#include <bw/alerts.h>
#include <bw/containers/latest_value.h>
#include <bw/input/input_types.h>
#include <bw/input/queue_command_sink.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for InputTask (device build only)

#include "wire_confirm.h"  // f14::onCommand - W3 P4 confirm/reject routing
#include "wire_hal.h"      // f01::buttons() - composition-level binding (README §1)
#include "wire_ui.h"       // f12::uiCommands() - UI-bound command routing (backlog 15)
#endif

namespace f11 {

namespace {

// Static-storage engine: constructed before any task runs - no heap, no
// init-order dependency. Everything configurable (debounce, long press, chord,
// stuck) is a named compile-time constant (bw/input/input_constants.h, F11
// §8), so nothing is injected at construction time.
bw::input::InputEngine s_engine{};

// --- P4 seam (F11 D8) ------------------------------------------------------
// AppContext gains `alert_status` and `commands` when the integrator lands
// P4 (after this feature reports; AppContext is integrator-owned, 01 §1 rule
// 3). The detection helpers below compile against BOTH shapes: today they
// yield null and the task defers exactly like F10's pump (Idle context,
// commands dropped), and they start returning the real fields the moment the
// fields exist - no edit here, no macro for anyone to remember to flip.

template <class C, class = void> struct CtxHasInputFields : std::false_type {};

template <class C>
struct CtxHasInputFields<C, std::void_t<decltype(std::declval<C&>().alert_status),
                                        decltype(std::declval<C&>().commands)>> : std::true_type {};

bw::LatestValue<bw::AlertStatus>* s_alert_mailbox = nullptr;
bw::ICommandSink* s_sink = nullptr;

// if constexpr in a TEMPLATE: the discarded branch is never instantiated, so
// `ctx.alert_status` / `ctx.commands` are only looked up when they exist.
template <class C> void bindCtxInput(C& ctx) {
    if constexpr (CtxHasInputFields<C>::value) {
        s_alert_mailbox = &ctx.alert_status;
        using CommandQueue = std::remove_reference_t<decltype(ctx.commands)>;
        static bw::input::QueueCommandSink<CommandQueue::capacity()> queue_sink{&ctx.commands};
        s_sink = &queue_sink;
    }
}

}  // namespace

#if defined(ARDUINO)

void inputTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: vTaskDelayUntil, never
    // delay()): the 100 Hz grid stays fixed even when one tick runs late, so
    // the debounce windows stay aligned with real time.
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // Clock read by the caller, never inside the module (F11 D6 / 01 §1):
        // no clock yet -> now = 0 is harmless, every duration is 0 until time
        // exists (tested state).
        const bw::TimeMs now = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;

        // D4 context: the latest published AlertStatus; an empty mailbox is
        // Idle - a tested state, not a guess. Alert staleness belongs to F10.
        //
        // Menu closure (backlog 32): the F12 menu is UI state no AlertStatus can
        // express, so the wiring carries it in as the second deriveContext
        // argument (the UI-state -> InputContext seam; bw_input still sees only
        // a bool). uiTask publishes it once per frame, so the newest value can
        // lag one 100 Hz tick behind a press that just opened the menu - at
        // human press rates (>100 ms apart) that never misroutes a gesture.
        // AlertActive outranks Menu inside deriveContext: a raised alert is
        // never blocked by UI state.
        bw::AlertStatus alert{};
        const bw::AlertStatus* alert_ptr =
            (s_alert_mailbox != nullptr && s_alert_mailbox->get(alert)) ? &alert : nullptr;
        const bw::input::InputContext ictx{bw::input::deriveContext(alert_ptr, f12::menuOpen())};

        // Composition-level binding: f01::wire ran first (README call order),
        // so the accessor is valid for the task's whole lifetime.
        const bw::IButtonInput& buttons = f01::buttons();
        const bool big = buttons.isPressedRaw(bw::ButtonId::Big);
        const bool small = buttons.isPressedRaw(bw::ButtonId::Small);

        const bw::input::StepOutput out = s_engine.step(now, ictx, big, small);

        // D6: push each routed command through the sink; a full queue refuses
        // (false) and the queue counts the drop - the gesture itself already
        // happened, never retried, never blocking (F11 §5).
        //
        // Backlog 15 resolved at W2 P4 (2026-10-12): screen-navigation
        // commands (NextScreen/OpenMenu - exactly the two the F12
        // ScreenManager consumes) go to F12's own port; every other command
        // keeps the documented f11 -> f10 path through ctx.commands. W3 P4
        // (2026-10-13) narrows that further: ConfirmEvent/RejectEvent go to
        // F14 here, because f10's per-tick drain consumes-and-ignores any
        // non-alert command (backlog 4's documented caveat) and F14 would
        // never see a button confirm. Single producer on each SPSC queue
        // (this task), and f12::uiCommands()/f14::onCommand are static
        // accessors, valid from wire onward regardless of wire order.
        for (uint8_t i = 0; i < out.count; ++i) {
            const bw::input::GestureRecord& gesture = out.items[i].gesture;

            // Menu closure (backlog 32): Menu·Big short and Menu·Small long are
            // deliberate "no command" cells (F11 §6 — no CommandType means
            // "select" or "back"), so route() emits nothing for them. The two
            // gestures travel as UI actions on F12's own port instead: the
            // small button navigates (short = scroll, long = back), the big
            // button acts (short = select). Never a Command, so the D2-N table
            // is unchanged and needs no CR (F11 §13).
            if (ictx.id == bw::input::InputContextId::Menu && !out.items[i].has_command &&
                !out.items[i].alert_consumed) {
                if (gesture.key == bw::input::GestureKey::BigShort) {
                    f12::postUiAction(f12::UiAction::Select);
                } else if (gesture.key == bw::input::GestureKey::SmallLong) {
                    f12::postUiAction(f12::UiAction::Back);
                }
                continue;
            }

            if (!out.items[i].has_command) {
                continue;
            }
            const bw::Command& cmd = out.items[i].command;
            if (cmd.type == bw::CommandType::ConfirmEvent ||
                cmd.type == bw::CommandType::RejectEvent) {
                // F14 §13: arg0 == 0 (the button convention) targets the
                // offered prompt; false = no prompt offered = no-op. The
                // gesture is never retried either way (D6, queue full).
                (void)f14::onCommand(cmd);
                continue;
            }
            if (s_sink == nullptr) {
                continue;
            }
            bw::ICommandSink* target = s_sink;
            if (cmd.type == bw::CommandType::NextScreen || cmd.type == bw::CommandType::OpenMenu) {
                bw::ICommandSink* const ui_sink = f12::uiCommands();
                if (ui_sink != nullptr) {
                    target = ui_sink;
                }
            }
            (void)target->push(cmd);  // F10 owns response acceptance and its timestamp
        }

        // Menu closure (backlog 32): the staged MenuAction the UI produced when
        // a selection landed. Only AcknowledgeAlert is app-level — it becomes a
        // Command on ctx.commands, whose documented producer is THIS task (app
        // .h: f11 → f10), so the SPSC rule holds. OpenDiagnostics / GoHome need
        // no Command: the ScreenManager already switched screens for them. The
        // action crosses as a semantic value, never a menu index, so F11 stays
        // ignorant of menu contents (F11 §12).
        const bw::ui::MenuAction staged = f12::takeMenuAction();
        if (staged == bw::ui::MenuAction::AcknowledgeAlert && s_sink != nullptr) {
            bw::Command ack{};
            ack.type = bw::CommandType::AcknowledgeAlert;
            ack.t_ms = now;
            ack.source = bw::input::kCommandSourceButton;
            (void)s_sink->push(ack);
        }

        // D5/§10: one Warning edge per stuck episode; drop while the reporter
        // is not wired yet (F01 D7 pattern - null is a tested state).
        for (uint8_t b = 0; b < 2; ++b) {
            if (out.stuck[b] && ctx.faults != nullptr) {
                ctx.faults->report(bw::FaultCode::ButtonStuck, bw::FaultSeverity::Warning, now);
            }
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Input, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kInputTaskPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Bind the P4 fields if they exist yet (see the seam above); no-op today.
    bindCtxInput(ctx);

#if defined(ARDUINO)
    // Task table (README §1, runtime.md §3.2): construction, injection and
    // task creation only - all behavior lives in bw_input, which the host
    // suite drives with scripted timelines.
    app::tasks::checkCreated(xTaskCreatePinnedToCore(inputTask, "InputTask", kInputTaskStackBytes,
                                                     &ctx, kInputTaskPriority, nullptr,
                                                     kInputTaskCore),
                             bw::diag::TaskSlot::Input, ctx.faults);
#else
    (void)ctx;
#endif
}

}  // namespace f11
