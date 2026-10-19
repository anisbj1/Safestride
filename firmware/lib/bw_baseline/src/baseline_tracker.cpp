#include <bw/baseline/baseline_tracker.h>

#include <cmath>
#include <limits>

namespace bw::baseline {

StepResult BaselineTracker::step(const BaselineSample& s) {
    const SampleVerdict gate = filter_.check(s);
    if (gate != SampleVerdict::Accepted) {
        // D5: (re)arm the cooldown on every held sample, so the hold's release
        // cannot sneak one sample in and a long event always pays the full
        // kPostEventHoldSamples cooldown after it clears.
        if (s.hold) {
            hold_cooldown_ = cfg_.post_event_hold_samples;
        }
        return {false, gate, false};
    }
    if (hold_cooldown_ > 0u) {
        // Cooldown counts potential-learning ticks (§8: 600 = 10 min at 1 Hz);
        // samples that fail other gates do not drain it, so 600 qualified
        // samples must pass before learning resumes (§9 test 8).
        --hold_cooldown_;
        return {false, SampleVerdict::HoldActive, false};
    }

    const bool was_valid = isValid();
    const uint32_t n = state_.accepted_samples;

    // D1 outlier gate: off during the seeding ramp, then reject anything beyond
    // max(floor, sigma * robust-deviation). Boundary inclusive — the plan's
    // "reject when > band" means a sample exactly at the band counts (§9 4).
    // ENMO rides the same sample-level decision: D1's state is exactly 3 floats
    // (no enmo absdev), and the quality/motion/activity gates already cover
    // motion artifacts in the ENMO channel (D1 trade-off).
    if (n >= cfg_.ewma_ramp_samples) {
        const float band =
            (std::fmax)(cfg_.min_outlier_band_bpm, cfg_.outlier_band_sigma * state_.hr_absdev_ewma);
        if (std::fabs(s.hr_bpm - state_.hr_ewma) > band) {
            return {false, SampleVerdict::Outlier, false};
        }
    }

    // D1 update rule: incremental-mean seeding while ramping, fixed window after.
    const float alpha = (n < cfg_.ewma_ramp_samples)
                            ? 1.0f / static_cast<float>(n + 1u)
                            : 1.0f / static_cast<float>(cfg_.ewma_steady_window_samples);
    const float hr_dev = std::fabs(s.hr_bpm - state_.hr_ewma);  // pre-update deviation
    state_.hr_ewma += alpha * (s.hr_bpm - state_.hr_ewma);
    state_.hr_absdev_ewma += alpha * (hr_dev - state_.hr_absdev_ewma);
    state_.enmo_ewma += alpha * (s.enmo_g - state_.enmo_ewma);
    // Saturate instead of wrap: at the 1 Hz learning rate u32 is 136 years,
    // and a wrap would drop n to 0 (restart the ramp, un-valid the profile) —
    // the counters must stay monotone for the device lifetime (00 §7.2).
    if (n < std::numeric_limits<uint32_t>::max()) {
        state_.accepted_samples = n + 1u;
    }

    const bool now_valid = isValid();
    return {true, SampleVerdict::Accepted, !was_valid && now_valid};
}

bool BaselineTracker::profile(bw::BaselineProfile& out) const {
    if (state_.accepted_samples == 0u) {
        return false;  // absent before any evidence (§9 test 1)
    }
    out.valid = isValid();
    out.resting_hr_bpm = state_.hr_ewma;
    out.resting_hr_mad = state_.hr_absdev_ewma;
    out.typical_enmo_g = state_.enmo_ewma;
    out.confidence_pct = confidencePct();
    out.version = kProfileVersion;
    return true;
}

bw::Result<void> BaselineTracker::restore(const TrackerState& in) {
    // A restored hr_ewma can only be a convex combination of in-band samples,
    // so anything outside the plausibility band (or non-finite) is garbage —
    // refuse it rather than publish a fabricated resting HR (00 §1).
    const bool hr_ok = std::isfinite(in.hr_ewma) && in.hr_ewma >= cfg_.min_resting_hr_bpm &&
                       in.hr_ewma <= cfg_.max_resting_hr_bpm;
    const bool mad_ok = std::isfinite(in.hr_absdev_ewma) && in.hr_absdev_ewma >= 0.0f;
    const bool enmo_ok = std::isfinite(in.enmo_ewma) && in.enmo_ewma >= 0.0f;
    if (!hr_ok || !mad_ok || !enmo_ok) {
        return {bw::Err::Corrupt};
    }
    state_ = in;
    hold_cooldown_ = 0u;  // fresh guard context after a restart (not persisted, D5)
    return {bw::Err::Ok};
}

bool BaselineTracker::isValid() const {
    const uint64_t qualified_ms =
        static_cast<uint64_t>(state_.accepted_samples) * static_cast<uint64_t>(kMsPerSecond);
    return qualified_ms >= static_cast<uint64_t>(cfg_.min_qualified_ms);
}

uint8_t BaselineTracker::confidencePct() const {
    if (cfg_.full_confidence_samples == 0u) {
        return 0u;  // defensive: a zero-width ramp would divide by zero
    }
    uint64_t pct = static_cast<uint64_t>(state_.accepted_samples) * kConfidencePctMax /
                   static_cast<uint64_t>(cfg_.full_confidence_samples);
    if (pct > kConfidencePctMax) {
        pct = kConfidencePctMax;
    }
    return static_cast<uint8_t>(pct);
}

}  // namespace bw::baseline
