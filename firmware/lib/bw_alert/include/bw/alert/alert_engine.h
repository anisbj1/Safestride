#pragma once

// bw/alert/alert_engine.h - F10 §7: the module the AlertTask ticks.
//
// AlertEngine = AlertPolicy (the rules) + PatternPlayer (the output) + one
// AlertRaised event per transition (F10 §3). It has no clock: time arrives as
// `t_ms`, so the whole feature is deterministic on the host and on the device.
//
// Deliberately NOT a task: this library contains no FreeRTOS/Arduino call and
// no provider lookup. The wiring pair (src/app/wiring/wire_alert.*) owns
// construction, and the pump that feeds snapshots/commands owns the mailbox
// and the IEventSink - that glue is composition, not alert logic (01 §1).

#include <bw/alert/alert_config.h>
#include <bw/alert/alert_patterns.h>
#include <bw/alert/alert_policy.h>
#include <bw/alert/pattern_player.h>
#include <bw/alerts.h>
#include <bw/events.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw::alert {

// The engine takes exactly what the policy takes (F10 §3 inputs): a snapshot,
// at most one event notification, at most one command, and the time.
using AlertInput = PolicyInput;

struct AlertOutput {
    DemoTelemetry demo{};
    bool wake_check_passed = false;
    // Current level, since when, why - ready for a LatestValue<AlertStatus>
    // mailbox that F12/F19 consume (F10 §3).
    AlertStatus status{};
    // True when the level changed this step; the AlertRaised event for that
    // change was published as part of this same call.
    bool level_changed = false;
    // Result of that publish: Ok when the level did not change or the sink
    // accepted it, NotReady when no sink is attached yet (deferred DI), or the
    // sink's own error. Never ignored (embedded conventions: a returned
    // Result is always observed) - the pump reports it through IFaultReporter.
    Err publish_err = Err::Ok;
};

class AlertEngine {
public:
    explicit AlertEngine(const AlertConfig& config = AlertConfig{}) : policy_(config) {}

    // Late dependency injection: the composition root attaches the real LED,
    // buzzer and event sink when it has instances (AppContext carries none of
    // them yet - F00 D9). nullptr detaches; a detached engine still decides
    // levels and still reports NotReady, it just cannot make light or noise.
    void attachOutputs(IRgbLed* led, IBuzzer* buzzer, IEventSink* events) {
        player_.attach(led, buzzer);
        events_ = events;
    }

    // One AlertTask tick (kAlertTickPeriodMs): evaluate the ladder, drive the
    // pattern player, publish an event if the level changed.
    AlertOutput step(const AlertInput& input);

    // Reads for tests, wiring and diagnostics; step() is the API.
    const AlertPolicy& policy() const { return policy_; }
    PatternPlayer& player() { return player_; }

private:
    AlertPolicy policy_;
    PatternPlayer player_;
    IEventSink* events_ = nullptr;
};

}  // namespace bw::alert
