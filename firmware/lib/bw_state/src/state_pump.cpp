// bw/state/state_pump.cpp — F07 §15 pump logic (spec D15): sequence-gated
// pulls, feature-quality staleness aging (kFeatureQualityStaleMs), F23
// quality-provider substitution (D16, P4b), engine step, publish + drop
// counting. Pure logic: no platform calls, no allocation, time passed in
// (D4).

#include <bw/state/state_pump.h>

namespace bw::state {

StatePump::StatePump(const StateEngineConfig& cfg) : engine_(cfg) {
    // D10: mode/power mirror the inputs; they default until an F24 provider
    // feeds the wiring, and behaviour does not depend on them.
    in_.mode = bw::OperatingMode::Day;
    in_.power = bw::PowerProfile::Normal;
}

namespace {

// True when a quality stamp is at most `limit` behind `now`. A backward or
// future stamp (violated monotonic contract, 01 §4) is not judged stale —
// the engine's own freshness check handles those axes; here it would only
// risk wiping a just-received quality.
bool aged(TimeMs now, TimeMs stamp, TimeMs limit) {
    return now >= stamp && (now - stamp) > limit;
}

}  // namespace

StateOutputs StatePump::tick(TimeMs now) {
    // A wake-check response is an edge-like mailbox value: only a new
    // sequence can be considered, and only for this tick. Its own timestamp
    // (not mailbox receipt time) must be in the current input freshness window.
    in_.wake_check_passed = false;
    in_.short_motion_burst = false;
    if (ports_.wake_check_passed != nullptr &&
        ports_.wake_check_passed->sequence() != seen_wake_check_passed_) {
        seen_wake_check_passed_ = ports_.wake_check_passed->sequence();
        bw::TimeMs passed_ms = 0;
        if (ports_.wake_check_passed->get(passed_ms) && passed_ms <= now &&
            (now - passed_ms) <= kInputStaleMs) {
            in_.wake_check_passed = true;
        }
    }

    // ---- pull: new values only (sequence-gated, F03's pattern) ------------
    if (ports_.motion != nullptr && ports_.motion->sequence() != seen_motion_) {
        seen_motion_ = ports_.motion->sequence();
        bw::MotionFeatures f{};
        if (ports_.motion->get(f)) {
            imu_quality_ = f.quality;
            imu_quality_stamp_ms_ = now;  // receipt time (A-03)
        }
    }
    if (ports_.ppg != nullptr && ports_.ppg->sequence() != seen_ppg_) {
        seen_ppg_ = ports_.ppg->sequence();
        bw::PpgFeatures f{};
        if (ports_.ppg->get(f)) {
            ppg_quality_ = f.quality;
            ppg_quality_stamp_ms_ = now;
        }
    }
    if (ports_.activity != nullptr && ports_.activity->sequence() != seen_activity_) {
        seen_activity_ = ports_.activity->sequence();
        bw::ActivityEstimate a{};
        if (ports_.activity->get(a)) {
            in_.activity = a;
        }
    }
    if (ports_.wear != nullptr && ports_.wear->sequence() != seen_wear_) {
        seen_wear_ = ports_.wear->sequence();
        bw::WearEstimate w{};
        if (ports_.wear->get(w)) {
            in_.wear = w;
        }
    }
    if (ports_.alertness != nullptr && ports_.alertness->sequence() != seen_alertness_) {
        seen_alertness_ = ports_.alertness->sequence();
        bw::AlertnessProbabilities a{};
        if (ports_.alertness->get(a)) {
            in_.alertness = a;
            in_.short_motion_burst =
                a.valid && (a.reasons & (static_cast<bw::ReasonMask>(1u) << static_cast<uint8_t>(
                                             bw::ReasonCode::ShortMotionBurst)));
        }
    }

    // ---- feature-quality staleness (auditor A-03; derivation channel) -----
    // A stopped F02 leaves its last (Good) quality latched in the mailbox;
    // the t_ms-based engine staleness covers wear/activity/alertness only,
    // because SourceQuality carries no stamp. So the pump ages the channel:
    // no new window within kFeatureQualityStaleMs ⇒ Insufficient. This
    // derivation runs untouched even while the provider is bound below —
    // it simply never feeds the engine then (D16 full substitution).
    if (aged(now, imu_quality_stamp_ms_, kFeatureQualityStaleMs)) {
        imu_quality_ = bw::DataQuality::Insufficient;
    }
    if (aged(now, ppg_quality_stamp_ms_, kFeatureQualityStaleMs)) {
        ppg_quality_ = bw::DataQuality::Insufficient;
    }

    // ---- quality sourcing: F23 provider substitution (D16, P4b) -----------
    // Bound: the provider IS the axis producer (context §6) — each `true`
    // return writes BOTH channels and stamps the receipt; `false`/silence
    // ages them back to Insufficient after kFeatureQualityStaleMs through
    // the same `aged()` machinery (no new constant, no retune). Absent: the
    // A-03 feature-mailbox derivation feeds the engine byte-for-byte
    // (missing ⇒ Insufficient, regression-critical — pinned by §9's pump
    // suite and the 4 P4b cases, H10).
    if (ports_.quality_provider != nullptr) {
        // Defensive seed (audit FIX loop 1): NOT `q{}` — value-initialization
        // would read as Good/Good (DataQuality::Good == 0), so a provider
        // returning true WITHOUT writing `out` would fabricate healthy data
        // from an unwritten out-param (00 §1 forbids exactly that). Seeded
        // Insufficient/Insufficient, an unwritten out-param is honest "no
        // reliable data"; a well-behaved provider overwrites both fields.
        bw::SourceQuality q{bw::DataQuality::Insufficient, bw::DataQuality::Insufficient};
        if (ports_.quality_provider(q)) {
            provider_quality_ = q;
            provider_quality_stamp_ms_ = now;  // receipt time (D16)
        }
        if (aged(now, provider_quality_stamp_ms_, kFeatureQualityStaleMs)) {
            provider_quality_.imu = bw::DataQuality::Insufficient;
            provider_quality_.ppg = bw::DataQuality::Insufficient;
        }
        in_.quality = provider_quality_;
    } else {
        in_.quality.imu = imu_quality_;
        in_.quality.ppg = ppg_quality_;
    }

    // ---- step (time as data, D4) ------------------------------------------
    // Absent/unpublished provider uses the original D10 defaults, not a held
    // previous profile. This metadata never alters evidence or gate priority.
    in_.mode = bw::OperatingMode::Day;
    in_.power = bw::PowerProfile::Normal;
    bw::ModeProfile profile{};
    if (ports_.mode_provider != nullptr && ports_.mode_provider->latest(profile)) {
        in_.mode = profile.mode;
        in_.power = profile.power;
    }
    in_.t_ms = now;

    const StateOutputs out = engine_.step(in_);

    // ---- publish (D11: a rejected/absent sink is the pump's problem) ------
    if (ports_.snapshot != nullptr) {
        ports_.snapshot->set(out.snapshot);
    }
    if (out.event_valid) {
        bool published = false;
        if (ports_.events != nullptr) {
            published = ports_.events->publish(out.event).ok();
        }
        if (!published) {
            // F17 absent (or rejected): count, never block — the record
            // stays reconstructable from the snapshot stream (D11).
            ++dropped_events_;
        }
    }

    last_run_ms_ = now;
    return out;
}

}  // namespace bw::state
