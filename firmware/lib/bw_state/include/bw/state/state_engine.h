#pragma once

// bw/state/state_engine.h — F07 §6/§7: the deterministic state machine that
// composes wear, activity, quality and alertness probabilities into one
// stable StateSnapshot (D1..D14). Pure logic: no platform calls, no
// allocation, time as data (D4) — step() is a pure function of its argument
// and compiles for native and esp32dev (01 §1 rule 5).
//
// Normative behaviour is the transition table in spec §7 (gates G1/G2/G3a,
// ladder rules E1..E4); the host tests mirror it row by row (§9, §14 DoD).

#include <bw/containers/latest_value.h>
#include <bw/providers.h>
#include <bw/state.h>
#include <bw/state/duration_counter.h>
#include <bw/state/smoother.h>
#include <bw/state/state_config.h>
#include <bw/state/state_types.h>

namespace bw::state {

class StateEngine {
public:
    explicit StateEngine(const StateEngineConfig& cfg = StateEngineConfig{});

    // One tick of the state machine. `in.t_ms` is "now" (read by the pump,
    // never by the engine — D4). Returns the snapshot plus, when the alertness
    // axis changed, the transition record the pump must publish (D11).
    StateOutputs step(const StateInputs& in);

    // Introspection for wiring/tests (§7 API sketch).
    AlertnessState state() const { return state_; }

private:
    // Gate/ladder helpers (implemented in state_engine.cpp).
    bool fresh(const StateInputs& in, TimeMs stamp, TimeMs limit) const;
    AlertnessState ladderStep(const StateInputs& in, ReasonCode& unknown_reason);
    void resetEvidence();  // D6: gate entry resets every evidence timer

    StateEngineConfig cfg_;

    // D7: two evidence channels — evidence_drowsy = EMA(drowsy + sleep)
    // feeds Drowsy/HighDrowsinessRisk and the E3 calm line; evidence_sleep =
    // EMA(sleep) feeds PossibleSleep only. Both seeded on the first valid
    // sample and reset at gate entry (D6/D7).
    Smoother evidence_drowsy_;
    Smoother evidence_sleep_;

    // D8/E1 escalation dwells (reset, never pause) + D8/E3 recovery dwell.
    DurationCounter run_drowsy_;
    DurationCounter run_high_risk_;
    DurationCounter run_sleep_;
    DurationCounter calm_;

    AlertnessState state_ = AlertnessState::Unknown;
    uint32_t in_state_ms_ = 0;    // saturating (kMaxDurationFieldMs)
    uint32_t duration_left_ = 0;  // duration of the state just left (D12)
    TimeMs last_t_ms_ = 0;
    bool have_step_ = false;
    // D6: the gate condition (G1 || G2) of the PREVIOUS step. Evidence resets
    // on this condition's rising edge, not on a state change — a gate blip
    // while already in a gate state must still wipe the evidence timers
    // (auditor A-01: the dwell may never straddle an unusable window).
    bool gate_active_ = false;
    // Why the engine is currently Unknown (G2 gate vs E4 insufficient
    // evidence): gate reason vs. InsufficientEvidence (CR-008 bit 14).
    ReasonCode unknown_reason_ = ReasonCode::SensorUnavailable;
};

// IStateProvider (02 §5) over the app-owned LatestValue<StateSnapshot>
// mailbox: F10/F12 and the bench read the newest snapshot without knowing
// F07. Bound by the wiring (nullptr until the mailbox exists).
class StateProvider final : public bw::IStateProvider {
public:
    void bind(const bw::LatestValue<bw::StateSnapshot>* mailbox) { snapshot_ = mailbox; }

    bool latest(bw::StateSnapshot& out) const override;

private:
    const bw::LatestValue<bw::StateSnapshot>* snapshot_ = nullptr;
};

}  // namespace bw::state
