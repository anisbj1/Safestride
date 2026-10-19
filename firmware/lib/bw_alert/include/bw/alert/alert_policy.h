#pragma once

// bw/alert/alert_policy.h - F10 §7: the rules of the alert ladder.
//
// Pure logic: `PolicyOutput evaluate(const PolicyInput&)` with time as an
// explicit argument, no platform calls, no hardware - so every row of the
// The policy never reads ML output or sensors (F10 §5): its inputs are a
// StateSnapshot, at most one event notification and at most one command.
//
// Invariant: at most ONE level transition per evaluate() call, so at most one
// AlertRaised event is produced per step (the AlertTask runs at 20 Hz, so a
// second transition is only ever deferred by 50 ms).

#include <bw/alert/alert_config.h>
#include <bw/alerts.h>
#include <bw/events.h>
#include <bw/state.h>

namespace bw::alert {

// One event notification forwarded by the wiring (F10 §3), presented for
// exactly one tick (§6.1 one-tick rule: holding it re-arms the hold window).
// Only the two types the rule table names are alert triggers (§6 D4);
// everything else is accepted and ignored rather than dropped, so the wiring
// needs no filter of its own.
struct EventNotification {
    bool present = false;
    // Read only when present is true; Boot is simply a neutral zero-ish default
    // (there is no "None" in the append-only EventType list, 02 §4.5).
    EventType type = EventType::Boot;
};

struct PolicyInput {
    TimeMs t_ms = 0;
    bool has_state = false;
    StateSnapshot state{};  // read only when has_state is true
    EventNotification notification{};
    bool acknowledge = false;  // Command{AcknowledgeAlert} (02 §4.6), one tick
    bool acknowledge_has_timestamp = false;
    TimeMs acknowledge_t_ms = 0;  // actual release; absent => evaluation time
    bool cancel = false;          // Command{CancelAlert} (02 §4.6), one tick
};

// Local diagnostics, not a persisted/shared contract. Absolute device times.
struct DemoTelemetry {
    bool monitoring = false;
    bool red_latched = false;
    TimeMs next_check_ms = 0;
    TimeMs response_deadline_ms = 0;
};

struct PolicyOutput {
    DemoTelemetry demo{};
    bool wake_check_passed = false;  // demo response accepted, not a safety-gate stop
    // Filled on every call: the current level, how long it has been active,
    // and why the engine is in it (02 §4.6).
    AlertStatus status{};
    // True when the level changed during this call. `event` is only meaningful
    // then: an AlertRaised record with payload[0] = AlertPhase (§6 D8) for the
    // transition, ready for IEventSink.
    bool level_changed = false;
    Event event{};
};

class AlertPolicy {
public:
    explicit AlertPolicy(const AlertConfig& config = AlertConfig{}) : config_(config) {}

    // Drives one evaluation at `input.t_ms`. Deterministic and side-effect free
    // apart from its own state; call it at kAlertTickPeriodMs cadence.
    // Input contract (F10 §6.1 one-tick rule): `acknowledge`, `cancel` and
    // `notification.present` are one-tick pulses. The policy deliberately does
    // not edge-detect them, so a level held across ticks re-applies each tick
    // (a held cancel pushes kCooldownMs forward every tick) - the pump must
    // present each command / notification exactly once.
    PolicyOutput evaluate(const PolicyInput& input);

    // Observability for tests and diagnostics; evaluate() is the API.
    AlertLevel level() const { return level_; }
    bool hasState() const { return have_state_; }
    bool isStale() const { return stale_; }

    // The OperatingMode profile currently in force (Day until a snapshot
    // arrives). The engine applies the SAME value to the output intensity as
    // the ceiling, so the two can never disagree (§6 D7).
    ModeIntensity profile() const;

private:
    PolicyOutput evaluateDemo(const PolicyInput& input);
    // Refreshes last_state_/stale_ from the input. A snapshot stamped ahead of
    // `t_ms` counts as stale: withinMs() rejects now < since, and "this input
    // cannot be trusted" is the safe reading (00 §1).
    void noteSnapshot(const PolicyInput& input);

    // Gates + state⇒level mapping (§6.1 rule table rows R1-R7). Sets `gated`
    // when no alert may be raised at all in this step (boot, stale, NotWorn,
    // Unknown) - the event floor is gated by it too (§6 D4, stage 1 - the
    // floor is a stage, not a row of the R1-R7 table).
    AlertLevel baseDemand(ReasonMask& reason, bool& gated) const;

    // §6 D4 event floor (stage 1 before the ladder, not a rule row): a held
    // notification floors the demand at its level, never lower.
    static AlertLevel floorFor(EventType type);

    // Performs the level transition, timestamps it and builds the event that
    // reports it. The only place `level_` may change.
    void changeTo(AlertLevel next, TimeMs now, ReasonMask reason, AlertPhase phase,
                  PolicyOutput& out);

    // AlertRaised record for one transition (§6 D8 payload layout).
    Event buildEvent(TimeMs now, AlertLevel level, AlertPhase phase, TimeMs start_ms,
                     ReasonMask reason) const;

    AlertConfig config_;

    // Newest snapshot + staleness.
    StateSnapshot last_state_{};
    bool have_state_ = false;
    TimeMs last_state_t_ms_ = 0;
    bool stale_ = false;

    // Held event notification (§6 D4).
    EventType notification_type_ = EventType::Boot;
    bool have_notification_ = false;
    TimeMs notification_since_ = 0;

    // Ladder state.
    AlertLevel level_ = AlertLevel::Normal;
    TimeMs level_since_ = 0;
    // Why the level currently in force was entered; frozen between
    // transitions so a long alert keeps explaining its own cause (F22).
    ReasonMask level_reasons_ = reasonBit(ReasonCode::SensorUnavailable);
    TimeMs persistent_since_ = 0;

    // De-escalation hysteresis (§6 D1).
    bool have_low_since_ = false;
    TimeMs low_since_ = 0;

    // Sustained-demand timer behind kWarningToActiveMs (§6.1 stage 6, §8):
    // `demand_since_` stamps when the demand at ITS CURRENT value started, so
    // `last_demand_` restarts it on every change of value - stage 6 only ever
    // measures continuous demand at the very value it acts on (Warning).
    AlertLevel last_demand_ = AlertLevel::Normal;
    TimeMs demand_since_ = 0;

    // Acknowledgment snooze (§6 D5): `ack_level_` is the level the user acked;
    // demands at or below it are ignored until `ack_until_`.
    bool acknowledged_ = false;
    AlertLevel ack_level_ = AlertLevel::Normal;
    TimeMs ack_until_ = 0;

    // Post-resolution / cancel quiet window (§8 kCooldownMs).
    TimeMs cooldown_until_ = 0;
    bool demo_monitoring_ = false;
    bool demo_still_ = false;
    bool demo_red_latched_ = false;
    TimeMs demo_interval_since_ = 0;
    TimeMs demo_session_since_ = 0;
    TimeMs demo_still_since_ = 0;
    TimeMs demo_evidence_ms_ = 0;
    TimeMs demo_motion_ms_ = 0;
    bool demo_have_motion_ = false;
};

}  // namespace bw::alert
