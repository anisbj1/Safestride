// bw/state/state_engine.cpp — F07: gates G1/G2/G3a (D3/D5/D6/D13), ladder
// rules E1..E4 (D8), evidence channels (D7), confidence + reason composition
// (D9/D14) and the transition records (D11/D12). Pure logic: no platform
// calls, no allocation, no globals.

#include <bw/state/state_engine.h>

#include <cmath>

namespace bw::state {
namespace {

// Ladder rung ordinals (AlertnessState numeric values are append-only, pinned
// by bw_core's contract test — 02 §4.5): escalation comparisons use them.
constexpr uint8_t kRungAwake = 2;
constexpr uint8_t kRungDrowsy = 3;
constexpr uint8_t kRungHighRisk = 4;
constexpr uint8_t kRungSleep = 5;

uint8_t rungOf(bw::AlertnessState s) {
    return static_cast<uint8_t>(s);
}

// Awake and above participate in the ladder; Unknown/NotWorn are gates (D3).
bool isRung(bw::AlertnessState s) {
    return rungOf(s) >= kRungAwake;
}

bw::ReasonMask bitOf(bw::ReasonCode c) {
    return static_cast<bw::ReasonMask>(1u << static_cast<uint8_t>(c));
}

// 02 §1 [0,1] clamp that maps NaN to 0: every comparison with NaN is false,
// so the first branch catches it — never a fabricated high confidence (D9).
float clamp01(float x) {
    if (!(x > 0.0f)) {
        return 0.0f;
    }
    if (x > kMaxProbability) {
        return kMaxProbability;
    }
    return x;
}

// D13: drowsy + sleep saturates at the [0,1] bound after validation.
float satProb(float x) {
    return x > kMaxProbability ? kMaxProbability : x;
}

// Saturating add for the uint32 duration fields (kMaxDurationFieldMs).
uint32_t satAdd32(uint32_t a, uint32_t b) {
    const uint64_t sum = static_cast<uint64_t>(a) + b;
    return sum > kMaxDurationFieldMs ? kMaxDurationFieldMs : static_cast<uint32_t>(sum);
}

// D13: probabilities must be finite and inside [0,1]; a NaN/1.5 probability
// (or a NaN confidence — it feeds the D9 product) makes the whole alertness
// input unusable. Never clamp into a plausible claim.
bool alertnessInputUsable(const bw::AlertnessProbabilities& a) {
    const auto in_range = [](float x) {
        return std::isfinite(x) && x >= 0.0f && x <= kMaxProbability;
    };
    return a.valid && in_range(a.awake) && in_range(a.drowsy) && in_range(a.sleep) &&
           std::isfinite(a.confidence);
}

}  // namespace

StateEngine::StateEngine(const StateEngineConfig& cfg)
    : cfg_(cfg), evidence_drowsy_(cfg.ema_alpha), evidence_sleep_(cfg.ema_alpha) {}

bool StateEngine::fresh(const StateInputs& in, TimeMs stamp, TimeMs limit) const {
    // A future stamp is "not fresh" (F04 treats it the same way): the value
    // cannot describe the present.
    return stamp <= in.t_ms && (in.t_ms - stamp) <= limit;
}

void StateEngine::resetEvidence() {
    // D6/D7 "reset": re-seed both smoothers, zero every counter.
    evidence_drowsy_.reset();
    evidence_sleep_.reset();
    run_drowsy_.reset();
    run_high_risk_.reset();
    run_sleep_.reset();
    calm_.reset();
}

AlertnessState StateEngine::ladderStep(const StateInputs& in, ReasonCode& unknown_reason) {
    // D7: two EMA channels, seeded on the first valid sample.
    const float ev_drowsy =
        evidence_drowsy_.step(satProb(in.alertness.drowsy + in.alertness.sleep));
    const float ev_sleep = evidence_sleep_.step(in.alertness.sleep);

    // D8/E1: dwells reset (never pause) on any sample below enter — evidence
    // must be continuous ("sustained").
    run_drowsy_.update(ev_drowsy >= cfg_.drowsy_enter_prob, in.t_ms);
    run_high_risk_.update(ev_drowsy >= cfg_.high_risk_enter_prob, in.t_ms);
    run_sleep_.update(ev_sleep >= cfg_.sleep_enter_prob, in.t_ms);
    calm_.update(ev_drowsy < cfg_.drowsy_exit_prob, in.t_ms);

    // E1 escalate: the highest rung whose enter threshold has been met
    // CONTINUOUSLY for its dwell. From a gate state the counters were reset
    // at entry, so the full rung dwell is paid (D6).
    uint8_t target = 0;
    if (run_sleep_.sustained(in.t_ms, cfg_.possible_sleep_min_duration_ms)) {
        target = kRungSleep;
    } else if (run_high_risk_.sustained(in.t_ms, cfg_.high_risk_min_duration_ms)) {
        target = kRungHighRisk;
    } else if (run_drowsy_.sustained(in.t_ms, cfg_.drowsy_min_duration_ms)) {
        target = kRungDrowsy;
    }
    if (target != 0 && target > rungOf(state_)) {
        return static_cast<AlertnessState>(target);
    }

    // Exit support (E2): nested bands (spec §7) guarantee a supported rung is
    // never ABOVE the current one when the current is unsupported.
    const bool sleep_supported = ev_sleep >= cfg_.sleep_exit_prob;
    const bool high_risk_supported = ev_drowsy >= cfg_.high_risk_exit_prob;
    const bool drowsy_supported = ev_drowsy >= cfg_.drowsy_exit_prob;

    if (isRung(state_)) {
        const bool current_supported = (state_ == AlertnessState::PossibleSleep) ? sleep_supported
                                       : (state_ == AlertnessState::HighDrowsinessRisk)
                                           ? high_risk_supported
                                           : drowsy_supported;
        if (current_supported) {
            return state_;  // E4 hold: hysteresis band (0.45 keeps Drowsy forever)
        }
        // E2 de-escalate immediately — the safe direction (D8).
        if (sleep_supported) {
            return AlertnessState::PossibleSleep;
        }
        if (high_risk_supported) {
            return AlertnessState::HighDrowsinessRisk;
        }
        if (drowsy_supported) {
            return AlertnessState::Drowsy;
        }
        // E3: recovery must ALSO be sustained (calm below the Drowsy exit).
        if (calm_.sustained(in.t_ms, cfg_.awake_recovery_ms)) {
            return AlertnessState::Awake;
        }
        return state_;  // E4 hold: calm dwell in progress, no fabricated Awake
    }

    // Gate-cleared path (G3b): the rung rules above apply from Awake, and
    // from Unknown only E1 (full dwell) or E3 (sustained calm) may fire.
    // E4: ambiguous-but-stable evidence stays Unknown with InsufficientEvidence
    // (D8/E4 — 00 §1 makes "unknown" a first-class output).
    unknown_reason = ReasonCode::InsufficientEvidence;
    if (calm_.sustained(in.t_ms, cfg_.awake_recovery_ms)) {
        return AlertnessState::Awake;
    }
    return state_;
}

StateOutputs StateEngine::step(const StateInputs& in) {
    StateOutputs out{};
    out.previous = state_;

    // D4: time as data. The first step has dt 0; a backward stamp cannot
    // happen through the monotonic contract (01 §4) and yields dt 0 anyway.
    TimeMs dt_ms = 0;
    if (have_step_ && in.t_ms >= last_t_ms_) {
        dt_ms = in.t_ms - last_t_ms_;
        if (dt_ms > kMaxDurationFieldMs) {
            dt_ms = kMaxDurationFieldMs;
        }
    }
    last_t_ms_ = in.t_ms;
    have_step_ = true;

    const bool wear_fresh = fresh(in, in.wear.t_ms, cfg_.input_stale_ms);
    const bool activity_fresh = fresh(in, in.activity.t_ms, cfg_.input_stale_ms);
    const bool alertness_fresh = fresh(in, in.alertness.t_ms, cfg_.alertness_stale_ms);
    const bool alertness_usable = alertnessInputUsable(in.alertness);

    // Gates in fixed priority (D3): NotWorn > Insufficient data > ladder.
    // A gate is only asserted on FRESH evidence: a stale WearEstimate may
    // not claim NotWorn (it falls to gate Unknown).
    const bool g1_not_worn = wear_fresh && in.wear.state == bw::WearState::NotWorn;
    const bool stale_trigger =
        !wear_fresh || !activity_fresh || !alertness_usable || !alertness_fresh;
    const bool quality_trigger = in.alertness.input_quality == bw::DataQuality::Insufficient ||
                                 (in.quality.imu == bw::DataQuality::Insufficient &&
                                  in.quality.ppg == bw::DataQuality::Insufficient);
    const bool wear_uncertain = wear_fresh && in.wear.state == bw::WearState::Uncertain;
    const bool g2_unknown = stale_trigger || quality_trigger || wear_uncertain;

    // D6 (normative): "timers reset at gate entry" means the gate's RISING
    // edge, not a state change. A gate blip while the engine is already in a
    // gate state (e.g. a one-tick both-sources-Insufficient while Unknown)
    // still declares the window unusable, so the dwell/EMA evidence must be
    // wiped then too — otherwise the calm/rung dwell would straddle a window
    // the engine itself refused to trust (auditor A-01). G3a is the mirrored
    // falling-edge case (NotWorn + gate clear) and resets below.
    const bool gate_active = g1_not_worn || g2_unknown;
    const bool gate_rising = gate_active && !gate_active_;
    gate_active_ = gate_active;
    if (gate_rising) {
        resetEvidence();
    }

    ReasonCode unknown_reason = unknown_reason_;
    AlertnessState next = state_;
    bool pre_reset = false;  // G3a reset evidence BEFORE this tick's timers
    bool wake_check_accepted = false;

    if (g1_not_worn) {
        next = AlertnessState::NotWorn;  // G1
    } else if (g2_unknown) {
        next = AlertnessState::Unknown;  // G2: reason is the stronger claim
        unknown_reason = stale_trigger     ? ReasonCode::SensorUnavailable
                         : quality_trigger ? ReasonCode::LowSignalQuality
                                           : ReasonCode::InsufficientEvidence;
    } else {
        // Gate condition clear (G3b): the ladder rules run, so evidence
        // timers advance every tick the gate stays clear (D6).
        if (state_ == AlertnessState::NotWorn) {
            // G3a: leaving NotWorn goes through Unknown, never straight to
            // Awake (D6). Reset BEFORE this tick's timer advance so the
            // recovery dwell starts exactly at gate entry (§9: Awake at
            // kAwakeRecoveryMs after entry). A just-reset rung/recovery run
            // cannot commit this tick; the table mandates Unknown here.
            next = AlertnessState::Unknown;
            unknown_reason = ReasonCode::InsufficientEvidence;
            resetEvidence();
            pre_reset = true;
        }
        const bool fresh_stillness_marker =
            in.short_motion_burst && alertness_usable && alertness_fresh &&
            in.alertness.input_quality != bw::DataQuality::Insufficient;
        if (fresh_stillness_marker) {
            // F06 emits this marker once, on the first still sample after
            // strong motion. Seed the EMA from that stillness evidence; the
            // ordinary rung dwell still starts and runs in ladderStep below.
            resetEvidence();
        }
        if (in.wake_check_passed && isRung(state_)) {
            // F10 accepts the response for the demo overlay, which can be
            // raised on any probabilistic rung; gates remain authoritative. Clear
            // all evidence so subsequent samples must rebuild any rung dwell.
            next = AlertnessState::Awake;
            wake_check_accepted = true;
            resetEvidence();
        } else {
            ReasonCode ladder_reason = unknown_reason_;
            const AlertnessState ladder_next = ladderStep(in, ladder_reason);
            if (!pre_reset) {
                next = ladder_next;
            }
            if (state_ == AlertnessState::Unknown) {
                unknown_reason = ladder_reason;  // E4 keeps/sets InsufficientEvidence
            }
        }
    }

    const bool changed = next != state_;
    if (changed) {
        out.transitioned = true;
        out.previous = state_;
        duration_left_ = satAdd32(in_state_ms_, static_cast<uint32_t>(dt_ms));
        state_ = next;
        in_state_ms_ = 0;
        // Gate-entry reset is the rising-edge reset above (D6): a transition
        // INTO a gate state always coincides with the gate becoming active,
        // so there is no second reset site. G3a (gate falling while NotWorn)
        // resets in its own branch, before this tick's timer advance.
        if (state_ == AlertnessState::Unknown) {
            unknown_reason_ = unknown_reason;
        }
    } else {
        in_state_ms_ = satAdd32(in_state_ms_, static_cast<uint32_t>(dt_ms));
        if (state_ == AlertnessState::Unknown) {
            unknown_reason_ = unknown_reason;  // gate reason may switch per step
        }
    }

    // ---- snapshot (D9/D10/D14) ---------------------------------------------
    bw::StateSnapshot& snap = out.snapshot;
    snap.t_ms = in.t_ms;
    snap.alertness = state_;
    // Stale axis mirrors describe the past -> Unknown, never a guess (D10).
    snap.wear = wear_fresh ? in.wear.state : bw::WearState::Uncertain;
    snap.activity = activity_fresh ? in.activity.context : bw::ActivityContext::Unknown;
    snap.quality = in.quality;  // mirror only; F07 does not re-decide (D10)
    snap.mode = in.mode;
    snap.power = in.power;
    snap.in_state_ms = in_state_ms_;
    if (!gate_active && isRung(state_) && in.quality.imu != DataQuality::Insufficient) {
        // CR-034: never restamp cached motion as a new observation.
        snap.wake_motion = in.alertness.wake_motion;
    }

    const bool degraded = in.quality.imu == bw::DataQuality::Degraded ||
                          in.quality.ppg == bw::DataQuality::Degraded ||
                          in.alertness.input_quality == bw::DataQuality::Degraded;
    // D9: a quarter off per degraded SOURCE (imu, ppg). alertness.input_quality
    // is deliberately not counted again: F06's own confidence already carries
    // its input-quality discount (no double counting).
    const uint8_t degraded_sources =
        static_cast<uint8_t>(in.quality.imu == bw::DataQuality::Degraded) +
        static_cast<uint8_t>(in.quality.ppg == bw::DataQuality::Degraded);

    float confidence = 0.0f;  // Unknown: 0 — never fabricate (00 §1)
    if (state_ == AlertnessState::NotWorn) {
        confidence = clamp01(in.wear.confidence);
    } else if (isRung(state_)) {
        float composed = clamp01(in.alertness.confidence) * clamp01(in.wear.confidence);
        for (uint8_t i = 0; i < degraded_sources; ++i) {
            composed *= cfg_.degraded_confidence_factor;
        }
        confidence = clamp01(composed);
    }
    snap.confidence = confidence;

    bw::ReasonMask reasons = 0;
    if (state_ == AlertnessState::Unknown) {
        reasons = bitOf(unknown_reason_);  // gate/E4 reason, never F06's bits
    } else if (state_ == AlertnessState::NotWorn) {
        reasons = bitOf(bw::ReasonCode::DeviceNotWorn);
    } else {
        // Ladder states: F07's own justification (D9 bit assignment) ...
        reasons =
            bitOf(wake_check_accepted ? bw::ReasonCode::UserMarked
                                      : (state_ == AlertnessState::Awake
                                             ? bw::ReasonCode::SustainedWakefulnessProbability
                                             : bw::ReasonCode::SustainedLowAlertnessProbability));
        if (degraded) {
            reasons |= bitOf(bw::ReasonCode::LowSignalQuality);
        }
        if (alertness_usable) {
            reasons |= in.alertness.reasons;  // ... plus F06's evidence (D14)
        }
    }
    snap.reasons = reasons;

    // ---- transition record (D11/D12) ---------------------------------------
    // Escalations only: DrowsinessEvent on entering Drowsy or
    // HighDrowsinessRisk, DeviceNotWorn on entering NotWorn. None for
    // Unknown (F23/F26 own sensor faults), Awake (visible in the snapshot
    // stream) or PossibleSleep (F08 owns SleepEpisode).
    bw::EventType type = bw::EventType::DrowsinessEvent;
    bool has_record = false;
    if (changed && state_ == AlertnessState::NotWorn) {
        type = bw::EventType::DeviceNotWorn;
        has_record = true;
    } else if (changed &&
               (state_ == AlertnessState::Drowsy || state_ == AlertnessState::HighDrowsinessRisk)) {
        type = bw::EventType::DrowsinessEvent;
        has_record = true;
    }

    if (has_record) {
        bw::Event& e = out.event;
        e = bw::Event{};                 // seq/boot_id/t_start_unix/payload tail stay 0 (D11/D12)
        e.t_start_ms = in.t_ms;          // transition time (D12)
        e.duration_ms = duration_left_;  // saturating duration of the state left
        e.confidence = snap.confidence;  // copied from the transition snapshot
        e.reasons = snap.reasons;
        e.quality = snap.quality;
        e.context = snap.activity;
        e.type = type;
        e.label = bw::UserLabel::Unlabeled;
        e.alertness_at_start = state_;    // the entered state (D12)
        e.detector_id = kStateFeatureId;  // 7, registry (D12)
        e.schema_version = bw::Event::kSchemaVersion;
        e.payload[kStatePayloadPrevIndex] = static_cast<uint8_t>(out.previous);
        e.payload[kStatePayloadNewIndex] = static_cast<uint8_t>(state_);
        out.event_valid = true;
    }

    return out;
}

bool StateProvider::latest(bw::StateSnapshot& out) const {
    if (snapshot_ == nullptr) {
        return false;  // no mailbox bound yet: "no snapshot yet" (02 §5)
    }
    return snapshot_->get(out);
}

}  // namespace bw::state
