#include "wire_alert.h"
#include "task_health.h"

#include <atomic>

#include "../app.h"

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for AlertTask (device build only)

#include "wire_hal.h"  // f01::led()/f01::buzzer() - composition-level binding
#endif

namespace f10 {

namespace {

// Static-storage engine (no heap: construction happens before any task runs,
// embedded conventions). Default thresholds live in bw::alert's named
// constants (F10 §8).
constexpr bw::alert::AlertConfig demoConfig() {
    bw::alert::AlertConfig config{};
    config.demo_wake_protocol = true;
    return config;
}
bw::alert::AlertEngine s_engine{demoConfig()};

// Pump-only drop counter (H7): written by alertTask, read by droppedEvents().
std::atomic<uint32_t> s_dropped_events{0};
bw::ICommandSink* s_mode_commands = nullptr;  // bound before AlertTask starts
std::atomic<uint32_t> s_dropped_mode_commands{0};

}  // namespace

uint32_t drainCommands(app::AppContext& ctx, bw::alert::PolicyInput& in,
                       bw::ICommandSink* mode_commands) {
    uint32_t refused = 0;
    bw::Command cmd{};
    while (ctx.commands.pop(cmd)) {
        switch (cmd.type) {
        case bw::CommandType::AcknowledgeAlert:
            in.acknowledge = true;
            in.acknowledge_has_timestamp = true;
            in.acknowledge_t_ms = cmd.t_ms;
            break;
        case bw::CommandType::CancelAlert:
            in.cancel = true;
            break;
        case bw::CommandType::SetMode:
        case bw::CommandType::StartNap:
        case bw::CommandType::EnterResearch:
        case bw::CommandType::ExitResearch:
            if (mode_commands == nullptr || !mode_commands->push(cmd)) {
                ++refused;
            }
            break;
        default:
            break;  // F15 and other command consumers remain deferred.
        }
    }
    return refused;
}

uint32_t droppedModeCommands() {
    return s_dropped_mode_commands.load(std::memory_order_relaxed);
}

#if defined(ARDUINO)

void alertTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2: no delay(), use
    // vTaskDelayUntil): the 20 Hz tick is also the pattern-step
    // quantisation bound (kAlertTickPeriodMs), so drift would show as
    // coarse alert patterns.
    TickType_t last_wake = xTaskGetTickCount();
    bw::TimeMs last_telemetry_ms = 0;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        // No clock yet: honest wait (F02/F07 rule) - levels and cooldowns
        // are timeline state, never a fabricated t = 0.
        if (ctx.clock != nullptr) {
            bw::alert::PolicyInput in{};  // AlertInput = PolicyInput (§3)
            in.t_ms = ctx.clock->nowMs();

            bw::StateSnapshot snapshot{};
            if (ctx.state_snapshot.get(snapshot)) {
                in.has_state = true;
                in.state = snapshot;
            }

            // One-tick command conversion (F10 §6.1, backlog 4): the
            // queue is drained every tick and each delivery becomes a
            // single-tick pulse - the policy does not edge-detect, so
            // holding one across ticks would push the cooldown forward
            // every tick. Mode commands relay to a separate F24 queue whose
            // sole producer is this task; never push back into ctx.commands.
            s_dropped_mode_commands.fetch_add(drainCommands(ctx, in, s_mode_commands),
                                              std::memory_order_relaxed);

            // in.notification.present stays false until F17 fans raised
            // events into the alert path (W3) - the event floor is then
            // driven by real EventTypes, never by a fabricated one.

            const bw::alert::AlertOutput out = s_engine.step(in);
            if (out.level_changed ||
                in.t_ms - last_telemetry_ms >= bw::alert::kDemoTelemetryPeriodMs) {
                last_telemetry_ms = in.t_ms;
                Serial.printf("[WAKECHECK] t=%llu monitoring=%u level=%u red=%u next_ms=%llu "
                              "deadline_ms=%llu\n",
                              static_cast<unsigned long long>(in.t_ms), out.demo.monitoring,
                              static_cast<unsigned>(out.status.level), out.demo.red_latched,
                              static_cast<unsigned long long>(out.demo.next_check_ms),
                              static_cast<unsigned long long>(out.demo.response_deadline_ms));
            }
            // Status on level change only: publishing every 50 ms tick
            // would inflate LatestValue::overwrites(), a real data-loss
            // counter (F00 D10), and `since`/duration are derivable.
            if (out.level_changed) {
                ctx.alert_status.set(out.status);
            }
            // F10 alone accepts responses, so an exact-deadline short press
            // cannot pre-emptively restore Awake in another task.
            if (out.wake_check_passed) {
                ctx.wake_check_passed_ms.set(in.t_ms);
            }
            // W6 P4 / CR-033: refused/missing event sink is counted AND
            // reported as EventPublishDrop; immediate local alerting continues.
            if (out.publish_err != bw::Err::Ok) {
                ++s_dropped_events;
                app::tasks::reportEventDrop(ctx.faults, in.t_ms);
            }
        }

        app::tasks::completeCycle(bw::diag::TaskSlot::Alert, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(bw::alert::kAlertTickPeriodMs));
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx, bw::ICommandSink* mode_commands) {
    s_mode_commands = mode_commands;
    // Construction only - s_engine already exists with default thresholds.
    // Task table landed at P4 (2026-10-12): create the 20 Hz AlertTask and
    // attach the bench actuators; the event sink stays null until F17 (a
    // detached engine still decides levels and still reports NotReady).
    // Before F07 publishes a snapshot the policy is gated (boot/no state):
    // Normal level, no fabricated alerts - a tested state (F10 §9).
#if defined(ARDUINO)
    attachOutputs(&f01::led(), &f01::buzzer(), nullptr);
    app::tasks::checkCreated(xTaskCreatePinnedToCore(alertTask, "F10Alert", kAlertTaskStackBytes,
                                                     &ctx, kAlertTaskPriority, nullptr,
                                                     kAlertTaskCore),
                             bw::diag::TaskSlot::Alert, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::alert::AlertEngine& engine() {
    return s_engine;
}

void attachOutputs(bw::IRgbLed* led, bw::IBuzzer* buzzer, bw::IEventSink* events) {
    s_engine.attachOutputs(led, buzzer, events);
}

uint32_t droppedEvents() {
    return s_dropped_events.load(std::memory_order_relaxed);
}

}  // namespace f10
