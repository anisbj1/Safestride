// bw/alert/alert_policy.cpp - F10 §6 rule table, evaluated top to bottom.

#include <bw/alert/alert_policy.h>

#include <bw/time.h>

namespace bw::alert {

namespace {

// Saturate into the uint32 duration fields (02 §4.5/§4.6, see
// kMaxDurationFieldMs): a >49-day uptime must not wrap to a tiny duration.
uint32_t saturatingMs32(TimeMs ms) {
    return (ms > kMaxDurationFieldMs) ? kMaxDurationFieldMs : static_cast<uint32_t>(ms);
}

}  // namespace

void AlertPolicy::noteSnapshot(const PolicyInput& input) {
    if (input.has_state) {
        last_state_ = input.state;
        have_state_ = true;
        last_state_t_ms_ = input.state.t_ms;
    }
    stale_ = have_state_ && !withinMs(input.t_ms, last_state_t_ms_, config_.state_stale_ms);
}

AlertLevel AlertPolicy::floorFor(EventType type) {
    switch (type) {
    // A suspected event is a stronger cue than the alertness state alone
    // (§6 D4); both are worded "suspected"/"possible" upstream (00 §1).
    case EventType::SuspectedCataplexy:
        return AlertLevel::Active;
    case EventType::DrowsinessEvent:
        return AlertLevel::Warning;
    default:
        return AlertLevel::Normal;
    }
}

AlertLevel AlertPolicy::baseDemand(ReasonMask& reason, bool& gated) const {
    gated = true;

    // R1: no data yet, or the newest snapshot is too old (§10). Hold exactly
    // kStateStaleMs - withinMs() is inclusive, so the boundary step is still
    // fresh - then no alert, with SensorUnavailable as the explanation.
    if (!have_state_ || stale_) {
        reason = reasonBit(ReasonCode::SensorUnavailable);
        return AlertLevel::Normal;
    }

    // R2: off the wrist ⇒ nothing to say. Wear and alertness are independent
    // axes (00 §6), so either one being "not worn" closes the gate.
    if (last_state_.wear == WearState::NotWorn ||
        last_state_.alertness == AlertnessState::NotWorn) {
        reason = reasonBit(ReasonCode::DeviceNotWorn);
        return AlertLevel::Normal;
    }

    // R3: Unknown means F07 could not decide; its own reasons say why.
    if (last_state_.alertness == AlertnessState::Unknown) {
        reason = last_state_.reasons;
        return AlertLevel::Normal;
    }

    // R4-R7: gate passed - the state axes map straight onto the ladder.
    gated = false;
    reason = last_state_.reasons;
    switch (last_state_.alertness) {
    case AlertnessState::Awake:  // R4
        return AlertLevel::Normal;
    case AlertnessState::Drowsy:  // R5
        return AlertLevel::Warning;
    case AlertnessState::HighDrowsinessRisk:  // R6
        return AlertLevel::Active;
    case AlertnessState::PossibleSleep:  // R7
        return AlertLevel::Persistent;
    default:
        return AlertLevel::Normal;
    }
}

ModeIntensity AlertPolicy::profile() const {
    const OperatingMode mode = have_state_ ? last_state_.mode : OperatingMode::Day;
    return config_.modeIntensity(mode);
}

void AlertPolicy::changeTo(AlertLevel next, TimeMs now, ReasonMask reason, AlertPhase phase,
                           PolicyOutput& out) {
    const TimeMs previous_start = level_since_;

    level_ = next;
    level_since_ = now;
    level_reasons_ = reason;
    have_low_since_ = false;
    if (next != AlertLevel::Normal) {
        acknowledged_ = false;  // a freshly raised alert is not acknowledged
        if (next == AlertLevel::Persistent) {
            persistent_since_ = now;  // §6 D3: the max-duration clock restarts
        }
    }

    // Raise ⇒ the alert starts now; stop ⇒ the record covers the alert that
    // just ended, so t_start/duration describe it (02 §4.5).
    const TimeMs start_ms = (next != AlertLevel::Normal) ? now : previous_start;
    out.level_changed = true;
    out.event = buildEvent(now, next, phase, start_ms, reason);
}

Event AlertPolicy::buildEvent(TimeMs now, AlertLevel level, AlertPhase phase, TimeMs start_ms,
                              ReasonMask reason) const {
    // Zero-initialised: seq/boot_id stay 0 (02 §4.5 assigns seq to F17; no
    // producer of boot_id exists yet - see the F10 status log), the reserved
    // payload tail stays 0, label stays Unlabeled.
    Event e{};
    e.t_start_ms = start_ms;
    e.duration_ms = saturatingMs32(elapsedMs(now, start_ms));
    e.t_start_unix = 0;  // F10 has no wall clock: 0 = unknown (02 §1)
    e.confidence = stale_ ? 0.0f : last_state_.confidence;
    e.reasons = reason;
    e.payload[kAlertPayloadPhaseIndex] = static_cast<uint8_t>(phase);
    e.payload[kAlertPayloadLevelIndex] = static_cast<uint8_t>(level);
    // A stale input cannot claim its sources were fine (00 §1: "no reliable
    // data" is a first-class output), and confidence must not be fabricated.
    e.quality = stale_ ? SourceQuality{DataQuality::Insufficient, DataQuality::Insufficient}
                       : last_state_.quality;
    e.type = EventType::AlertRaised;
    e.label = UserLabel::Unlabeled;
    e.context = last_state_.activity;
    e.alertness_at_start = last_state_.alertness;
    e.detector_id = kAlertFeatureId;
    e.schema_version = Event::kSchemaVersion;
    return e;
}

PolicyOutput AlertPolicy::evaluate(const PolicyInput& in) {
    if (config_.demo_wake_protocol) {
        return evaluateDemo(in);
    }
    PolicyOutput out{};
    const TimeMs now = in.t_ms;

    // --- 1. inputs -----------------------------------------------------------
    noteSnapshot(in);
    ReasonMask reason = 0;
    bool gated = false;
    AlertLevel demand = baseDemand(reason, gated);

    // --- 2. event floor (§6 D4), only while the gate is open ----------------
    if (in.notification.present) {
        notification_type_ = in.notification.type;
        notification_since_ = now;
        have_notification_ = true;
    }
    if (have_notification_ && elapsedMs(now, notification_since_) > config_.event_alert_hold_ms) {
        have_notification_ = false;
    }
    if (!gated && have_notification_ && floorFor(notification_type_) > demand) {
        demand = floorFor(notification_type_);
    }

    // --- 3. commands outrank the rules (§6 D5) ------------------------------
    if (in.cancel) {
        acknowledged_ = false;
        ack_level_ = AlertLevel::Normal;
        ack_until_ = 0;
        cooldown_until_ = now + config_.cooldown_ms;
        if (level_ != AlertLevel::Normal) {
            changeTo(AlertLevel::Normal, now, reason, AlertPhase::Cancelled, out);
        }
    }
    if (in.acknowledge && level_ != AlertLevel::Normal) {
        ack_level_ = level_;
        ack_until_ = now + config_.ack_snooze_ms;
        acknowledged_ = true;
        changeTo(AlertLevel::Normal, now, reason, AlertPhase::Acknowledged, out);
    }

    // --- 4. input lost, then max-duration give-up (§10, §6 D3) --------------
    if (!out.level_changed && stale_ && level_ != AlertLevel::Normal) {
        // No cooldown here: the condition may be unchanged, so a fresh
        // snapshot must be able to raise again immediately.
        changeTo(AlertLevel::Normal, now, reasonBit(ReasonCode::SensorUnavailable),
                 AlertPhase::StateStale, out);
    } else if (!out.level_changed && level_ == AlertLevel::Persistent &&
               elapsedMs(now, persistent_since_) >= config_.persistent_max_ms) {
        changeTo(AlertLevel::Normal, now, reason, AlertPhase::AutoStoppedMaxDuration, out);
        cooldown_until_ = now + config_.cooldown_ms;
    }

    // One transition per step: the remaining demand is re-evaluated on the
    // next tick (<= kAlertTickPeriodMs later), so no event is ever lost.
    if (!out.level_changed) {
        // --- 5. suppression windows (§6 D5, §8) -----------------------------
        // Ack snooze: demands at or BELOW the acknowledged level are ignored;
        // a higher level still raises - an ack may never block escalation.
        if (acknowledged_ && now < ack_until_ && demand <= ack_level_) {
            demand = AlertLevel::Normal;
        }
        if (now < cooldown_until_) {
            demand = AlertLevel::Normal;
        }

        // --- 6. sustained demand: Warning held kWarningToActiveMs ⇒ Active -
        // §6.1 stage 6 acts on the Warning demand only, and it measures that
        // demand at ITS current value: `last_demand_` restarts the timer on
        // every change of value, so the stage fires after kWarningToActiveMs of
        // CONTINUOUS Warning demand - time spent earlier under another demand
        // (e.g. Active) never counts towards it, which keeps an Active ⇒
        // Warning step-down (stage 8) reachable and re-escalation on time.
        if (demand != last_demand_) {
            last_demand_ = demand;
            demand_since_ = now;
        }
        if (demand == AlertLevel::Warning &&
            elapsedMs(now, demand_since_) >= config_.warning_to_active_ms) {
            demand = AlertLevel::Active;
        }

        // --- 7. mode ceiling last, so it also caps the escalation (§6 D7) ---
        const ModeIntensity mode_profile = profile();
        if (demand > mode_profile.max_level) {
            demand = mode_profile.max_level;
        }

        // --- 8. ladder: up immediately, down only after the hold (§6 D1) ----
        if (demand > level_) {
            changeTo(demand, now, reason, AlertPhase::Raised, out);
        } else if (demand < level_) {
            if (!have_low_since_) {
                low_since_ = now;
                have_low_since_ = true;
            } else if (elapsedMs(now, low_since_) >= config_.deescalate_hold_ms) {
                changeTo(demand, now, reason,
                         demand == AlertLevel::Normal ? AlertPhase::Resolved : AlertPhase::Raised,
                         out);
                if (demand == AlertLevel::Normal) {
                    cooldown_until_ = now + config_.cooldown_ms;
                }
            }
        } else {
            have_low_since_ = false;
        }
    }

    // --- 9. status every caller sees (02 §4.6) ------------------------------
    // Reasons semantics: while the level is Normal they track the CURRENT
    // evaluation (so a gate such as "not worn" is visible even though nothing
    // was raised); once a level is in force they freeze at that level's entry
    // reasons, so a long alert keeps explaining its own cause (F22).
    if (level_ == AlertLevel::Normal && !out.level_changed) {
        level_reasons_ = reason;
    }
    out.status.t_ms = now;
    out.status.level = level_;
    out.status.acknowledged = acknowledged_ && (now < ack_until_);
    out.status.reasons = stale_ ? reasonBit(ReasonCode::SensorUnavailable) : level_reasons_;
    out.status.since_ms = saturatingMs32(elapsedMs(now, level_since_));
    return out;
}

PolicyOutput AlertPolicy::evaluateDemo(const PolicyInput& in) {
    PolicyOutput out{};
    const TimeMs now = in.t_ms;
    noteSnapshot(in);
    ReasonMask reason = 0;
    bool gated = false;
    (void)baseDemand(reason, gated);
    // Demo evidence is IMU-based. An uncertain wear or missing IMU must not
    // establish a monitoring session or carry a stillness run across a gap.
    gated = gated || last_state_.wear != WearState::Worn ||
            last_state_.quality.imu == DataQuality::Insufficient;
    const bool new_evidence =
        in.has_state && (!demo_monitoring_ || last_state_.t_ms != demo_evidence_ms_);
    bool motion = false;
    if (gated) {
        demo_monitoring_ = false;
        demo_still_ = false;
        demo_red_latched_ = false;
        acknowledged_ = false;
        if (level_ != AlertLevel::Normal) {
            changeTo(AlertLevel::Normal, now, reason,
                     stale_ ? AlertPhase::StateStale : AlertPhase::Resolved, out);
        }
    } else {
        if (!demo_monitoring_ || now < demo_interval_since_) {
            if (now < demo_interval_since_) {
                demo_have_motion_ = false;
            }
            demo_monitoring_ = true;
            demo_interval_since_ = now;
            demo_session_since_ = now;
            demo_still_ = false;
        }
        if (new_evidence) {
            const bool continuous = last_state_.t_ms >= demo_evidence_ms_ &&
                                    last_state_.t_ms - demo_evidence_ms_ <= kDemoEvidenceGapMs;
            if (!continuous) {
                demo_still_ = false;
            }
            const bool low = (reason & reasonBit(ReasonCode::SustainedLowMotion)) != 0;
            const bool burst = (reason & reasonBit(ReasonCode::ShortMotionBurst)) != 0;
            if (!low) {
                demo_still_ = false;
            } else if (burst && !demo_still_) {
                demo_still_ = true;
                demo_still_since_ = last_state_.t_ms;
            }
            demo_evidence_ms_ = last_state_.t_ms;
        }
        const WakeMotionEvidence& evidence = last_state_.wake_motion;
        if (evidence.valid && evidence.t_ms <= now &&
            now - evidence.t_ms <= kDemoMotionFreshnessMs && evidence.t_ms >= demo_session_since_ &&
            (!demo_have_motion_ || evidence.t_ms > demo_motion_ms_)) {
            demo_have_motion_ = true;
            demo_motion_ms_ = evidence.t_ms;
            motion = evidence.qualifying;
        }
        if (motion && !demo_red_latched_) {
            demo_still_ = false;
            // A newly consumed observation restarts the full normal interval.
            // Cached F07 snapshots must never extend it again. Use the session
            // boundary above, NOT the previous reset's consumption time: new
            // delayed windows can predate that reset while still being newer
            // observations than the last consumed movement.
            demo_interval_since_ = now;
        }
        // Use actual release time: a timely queued response must not lose to
        // 50 ms of task quantisation. Exactly at the deadline is too late.
        const TimeMs ack_time = in.acknowledge_has_timestamp ? in.acknowledge_t_ms : now;
        const bool timely_ack = !demo_red_latched_ && in.acknowledge &&
                                level_ == AlertLevel::Warning && ack_time >= level_since_ &&
                                ack_time <= now &&
                                elapsedMs(ack_time, level_since_) < config_.demo_response_window_ms;
        const bool expired = level_ == AlertLevel::Warning &&
                             elapsedMs(now, level_since_) >= config_.demo_response_window_ms &&
                             !timely_ack;
        if (expired) {
            demo_red_latched_ = true;
        }
        const bool answered = !demo_red_latched_ && level_ == AlertLevel::Warning && !expired &&
                              (timely_ack || (motion && evidence.t_ms >= level_since_));
        if (in.cancel || answered) {
            demo_red_latched_ = false;
            demo_still_ = false;
            demo_interval_since_ = now;
            acknowledged_ = timely_ack && !in.cancel;
            ack_until_ = now + config_.demo_check_interval_ms;
            if (level_ != AlertLevel::Normal) {
                out.wake_check_passed = true;
                changeTo(AlertLevel::Normal, now, reason,
                         in.cancel    ? AlertPhase::Cancelled
                         : timely_ack ? AlertPhase::Acknowledged
                                      : AlertPhase::Resolved,
                         out);
            }
        } else {
            const bool periodic =
                elapsedMs(now, demo_interval_since_) >= config_.demo_check_interval_ms;
            const bool still = demo_still_ && elapsedMs(demo_evidence_ms_, demo_still_since_) >=
                                                  config_.demo_burst_stillness_ms;
            AlertLevel desired = demo_red_latched_ ? AlertLevel::Active
                                 : (level_ == AlertLevel::Warning || periodic || still)
                                     ? AlertLevel::Warning
                                     : AlertLevel::Normal;
            const AlertLevel ceiling = profile().max_level;
            if (desired > ceiling) {
                desired = ceiling;
            }
            if (desired != level_) {
                changeTo(desired, now, reason,
                         desired == AlertLevel::Normal ? AlertPhase::Resolved : AlertPhase::Raised,
                         out);
            }
        }
    }
    if (level_ == AlertLevel::Normal && !out.level_changed) {
        level_reasons_ = reason;
    }
    out.status = {now, level_, acknowledged_ && now < ack_until_, level_reasons_,
                  saturatingMs32(elapsedMs(now, level_since_))};
    out.demo.monitoring = demo_monitoring_;
    out.demo.red_latched = demo_red_latched_;
    if (demo_monitoring_ && !demo_red_latched_) {
        if (level_ == AlertLevel::Warning) {
            out.demo.response_deadline_ms = level_since_ + config_.demo_response_window_ms;
        } else {
            out.demo.next_check_ms = demo_interval_since_ + config_.demo_check_interval_ms;
        }
    }
    return out;
}

}  // namespace bw::alert
