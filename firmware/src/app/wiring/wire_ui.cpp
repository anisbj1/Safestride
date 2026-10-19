#include "wire_ui.h"
#include "task_health.h"

#include <type_traits>
#include <utility>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/containers/spsc_queue.h>
#include <bw/providers.h>
#include <bw/ui/screens.h>
#include <bw/ui/ui_model.h>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for UiTask (device build only)

#include "wire_hal.h"  // f01::display()/f01::battery() — composition-level binding
#endif

namespace f12 {

namespace {

// Static storage: constructed before any task runs — no heap, no init-order
// dependency (same pattern as wire_input.cpp). Behavior lives in bw_ui.
bw::ui::UiInputs s_inputs{};
constexpr bw::ui::UiConfig demoConfig() {
    auto config = bw::ui::defaultUiConfig();
    config.demo_wake_protocol = true;
    return config;
}
bw::ui::UiModel s_model{demoConfig()};
bw::ui::ScreenRenderer s_renderer{};

// D4: F12's own SPSC queue and sink. F12 NEVER pops ctx.commands (its
// documented pair is f11 → f10); whoever the integrator routes pushes
// UI-bound commands into this port instead.
bw::SpscQueue<bw::Command, kUiCommandQueueLen> s_command_queue{};

// Menu closure (backlog 32): two more SPSC queues plus one mailbox, each with
// its documented single producer, so no queue ever gains a second writer:
//   s_action_queue      InputTask -> uiTask    (UiAction: Select / Back)
//   s_staged_action     uiTask -> InputTask    (MenuAction, drained by f11)
//   s_menu_open         uiTask -> InputTask    (LatestValue<bool>, context)
// The staged action round-trips because ctx.commands' ONLY producer is
// InputTask (app.h); letting uiTask push there would make it two producers on
// one SPSC queue. F11 converts the action to a Command on its own side, which
// also keeps menu CONTENTS out of F11 (§12: it sees "AcknowledgeAlert", never
// "entry index 2").
bw::SpscQueue<UiAction, kUiActionQueueLen> s_action_queue{};
bw::SpscQueue<bw::ui::MenuAction, kUiActionQueueLen> s_staged_action_queue{};
bw::LatestValue<bool> s_menu_open{};

class UiCommandSink final : public bw::ICommandSink {
public:
    bool push(const bw::Command& cmd) override { return s_command_queue.push(cmd) == bw::Err::Ok; }
};

UiCommandSink s_sink{};

// W3 P4 (2026-10-13, CR-016) + backlog 27: F14's prompt provider. Stored
// directly in s_inputs.prompt, so there is exactly ONE copy of this binding —
// no second pointer to keep in step. nullptr = the W2 default (Prompt screen
// unreachable via the D6 gate); the composition root attaches &f14::manager()
// after f14::wire. UiModel::readPrompt consumes it every frame and owns the
// D6 availability gate from that same read.

// --- P4-style SFINAE seam (same idiom as wire_input.cpp) --------------------
// AppContext is integrator-owned (01 §1 rule 3) and gains fields as features
// land. The fields that exist today are bound directly; the F23 health
// mailbox may arrive under either candidate name and is attached the moment
// one of them exists — no edit here, no macro for anyone to remember.

template <class C, class = void> struct CtxHasHealthField : std::false_type {};

template <class C>
struct CtxHasHealthField<C, std::void_t<decltype(std::declval<C&>().health)>> : std::true_type {};

template <class C, class = void> struct CtxHasHealthSnapshotField : std::false_type {};

template <class C>
struct CtxHasHealthSnapshotField<C, std::void_t<decltype(std::declval<C&>().health_snapshot)>>
    : std::true_type {};

template <class C> void bindCtxMailboxes(C& ctx) {
    // Present-today mailboxes (app.h, W1): f07 → state, f10 → alert, f02 → ppg.
    s_inputs.state = &ctx.state_snapshot;
    s_inputs.alert = &ctx.alert_status;
    s_inputs.ppg = &ctx.ppg_features;
    s_inputs.clock = ctx.clock;  // nullptr-safe per D5: timeouts/staleness disabled
    if constexpr (CtxHasHealthField<C>::value) {
        s_inputs.health = &ctx.health;
    } else if constexpr (CtxHasHealthSnapshotField<C>::value) {
        s_inputs.health = &ctx.health_snapshot;
    }
}

}  // namespace

bw::ICommandSink* uiCommands() {
    return &s_sink;
}

void postUiAction(UiAction action) {
    (void)s_action_queue.push(action);
}

bool menuOpen() {
    bool open = false;
    (void)s_menu_open.get(open);
    return open;
}

bw::ui::MenuAction takeMenuAction() {
    bw::ui::MenuAction action = bw::ui::MenuAction::None;
    (void)s_staged_action_queue.pop(action);
    return action;
}

// One pump of the input seam: drain this tick's UI-bound Commands into the
// model, then apply every queued menu gesture, forward any staged semantic
// action toward f11, and publish menuOpen(). uiTask calls it once per frame;
// the host suite calls it directly (it lives outside the ARDUINO guard on
// purpose — 01 §1 rule 5, pure logic must run without hardware).
void pumpInputs(bw::TimeMs now) {
    bw::Command cmd{};
    while (s_command_queue.pop(cmd)) {
        s_model.handleCommand(cmd, now);
    }

    UiAction action{};
    while (s_action_queue.pop(action)) {
        switch (action) {
        case UiAction::Select:
            s_model.screens().menuSelect();
            break;
        case UiAction::Back:
            s_model.screens().menuBack();
            break;
        }
    }

    // Hand the staged action back to InputTask (see the queue block above for
    // why it round-trips): only f11 may push to ctx.commands, so only f11 can
    // turn "AcknowledgeAlert" into a Command. None is the normal case — most
    // pumps stage nothing.
    const bw::ui::MenuAction staged = s_model.takeAction();
    if (staged != bw::ui::MenuAction::None) {
        (void)s_staged_action_queue.push(staged);
    }

    // Publish the UI-state half of the context seam: F11's InputTask reads this
    // to route the NEXT press as Idle vs Menu. One set per pump, and the 100 Hz
    // reader drains faster than the 10 Hz writer, so no unread value is ever
    // overwritten (overwrites() stays a real data-loss counter, F00 D10).
    s_menu_open.set(s_model.screens().menuOpen());
}

void attachConfirmPromptProvider(bw::IConfirmPromptProvider* provider) {
    s_inputs.prompt = provider;
}

void wire(app::AppContext& ctx) {
    bindCtxMailboxes(ctx);

#if defined(ARDUINO)
    // Composition-level bindings: f01::wire ran first (wiring README call
    // order), so the accessors are valid for the task's whole lifetime.
    s_inputs.battery = &f01::battery();
    // nullptr until the integrator assigns F26's registry (§10 fallback).
    s_renderer.attachFaultReporter(ctx.faults);

    app::tasks::checkCreated(xTaskCreatePinnedToCore(uiTask, "UiTask", kUiTaskStackBytes, &ctx,
                                                     kUiTaskPriority, nullptr, kUiTaskCore),
                             bw::diag::TaskSlot::Ui, ctx.faults);
#else
    (void)ctx;  // host: mailboxes bound, no task, no HAL (D5)
#endif
}

#if defined(ARDUINO)

void uiTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);
    (void)ctx;

    // Absolute-deadline pacing (runtime.md §3.2: vTaskDelayUntil, never
    // delay()): the 10 fps grid stays fixed even when one tick runs late.
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        const bw::TimeMs now = (s_inputs.clock != nullptr) ? s_inputs.clock->nowMs() : 0;

        // D4: drain this tick's UI commands into the model; an empty queue is
        // the W2 default (nothing routes yet) and a tested state. Then apply
        // the menu gestures (backlog 32), forward any staged MenuAction, and
        // publish the menu state f11 derives its InputContext from. All of it
        // lives in pumpInputs(), outside the ARDUINO guard, so the host suite
        // drives the identical path (01 §1 rule 5).
        //
        // The CR-016 prompt provider is bound into s_inputs (backlog 27), so
        // UiModel::readPrompt consumes it here too — and the model now owns the
        // D6 availability gate from that same read, so there is no second poll
        // to keep in step.
        pumpInputs(now);

        // D8: one frame per tick while awake; a timed-out screen presents
        // nothing at all (the renderer only tracks wake conditions).
        s_renderer.render(s_model.step(s_inputs, now), f01::display(), now);

        app::tasks::completeCycle(bw::diag::TaskSlot::Ui, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kUiTaskPeriodMs));
    }
}

#endif  // ARDUINO

}  // namespace f12
