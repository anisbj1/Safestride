#include <bw/signal/ppg_beat_detector.h>

namespace bw::signal {

void PpgBeatDetector::reset() {
    beats_.clear();
    prev2_x_ = 0.0f;
    prev1_x_ = 0.0f;
    have_two_prev_ = false;
    dc_ema_ = 0.0f;
    have_dc_ = false;
    have_locked_peak_ = false;
    locked_t_ms_ = 0;
    locked_amp_ = 0.0f;
    last_beat_t_ms_ = 0;
    have_last_beat_ = false;
    obs_ = Observability{};
}

bool PpgBeatDetector::push(float x, uint32_t dc_raw, TimeMs t_ms) {
    // DC estimate: tau ~ 1 s (kPpgDcEmaAlpha) — slower than any beat so the
    // noise floor does not chase the pulse (F02 D6). Seeded by the first
    // sample so the floor is correct from sample one.
    if (!have_dc_) {
        dc_ema_ = static_cast<float>(dc_raw);
        have_dc_ = true;
    } else {
        dc_ema_ += kPpgDcEmaAlpha * (static_cast<float>(dc_raw) - dc_ema_);
    }

    // FIX loop 4 (bench round 7): the detection threshold is the PI noise
    // floor ONLY. The old adaptive term (kPeakAdaptiveFraction x amp_ema_,
    // an EMA over accepted beats) was a RATCHET: real beat amplitudes vary
    // 30–100 % beat-to-beat, so one strong beat lifted the bar above the
    // next weak REAL beat → missed → consistency collapsed → "--" until the
    // starvation watchdog re-seeded — an on/off cycle under steady contact
    // (round 7; jitter fixture: 34/60 valid windows before this fix). The
    // amplitude state and the watchdog are DELETED: detection cannot depend
    // on recent amplitudes, so neither self-locking (round 3) nor ratcheting
    // (round 7) can recur. Noise above the floor can fabricate beats — the
    // already-accepted watchdog-floor trade; honesty lives downstream
    // (missed-beat-tolerant IBI consistency, SQI, motion flag; D5: the
    // state that caused the failure class no longer exists).
    const float threshold = kMinPerfusionIndex * dc_ema_;  // 0.1 % PI floor

    // Tallest-in-window flush (FIX loop 4): the tallest floor-clearing peak
    // kept inside the refractory lockout is accepted — stamped with its own
    // time — the moment the lockout expires. Accepting it may leave this
    // sample still inside the NEW beat's lockout, so the flag is recomputed
    // before the current peak is considered.
    bool past_lockout = !have_last_beat_ || t_ms >= last_beat_t_ms_ + kPpgRefractoryMs;
    if (have_locked_peak_ && past_lockout) {
        beats_.push(locked_t_ms_);
        last_beat_t_ms_ = locked_t_ms_;
        have_last_beat_ = true;
        have_locked_peak_ = false;
        past_lockout = t_ms >= last_beat_t_ms_ + kPpgRefractoryMs;
    }

    const bool is_peak = have_two_prev_ && prev1_x_ >= prev2_x_ && prev1_x_ > x;
    // FIX-2 observability: tallied from the same rule/threshold, before the
    // refractory gate, saturating (a window holds ~100 samples; the cap is
    // far above any physical count and must never wrap).
    obs_.last_threshold = threshold;
    if (is_peak) {
        if (obs_.candidates < 0xFFFFu) {
            ++obs_.candidates;
        }
        if (prev1_x_ > threshold) {
            if (obs_.peaks_above < 0xFFFFu) {
                ++obs_.peaks_above;
            }
        }
        if (prev1_x_ > obs_.strongest_peak) {
            obs_.strongest_peak = prev1_x_;
        }
    }
    bool beat = false;
    if (is_peak && prev1_x_ > threshold) {
        if (past_lockout) {
            beats_.push(t_ms);
            last_beat_t_ms_ = t_ms;
            have_last_beat_ = true;
            beat = true;
        } else if (!have_locked_peak_ || prev1_x_ > locked_amp_) {
            // Inside the lockout: not a beat, but keep the tallest candidate
            // (dicrotic notch vs weak systolic peak — see the header).
            locked_t_ms_ = t_ms;
            locked_amp_ = prev1_x_;
            have_locked_peak_ = true;
        }
    }

    prev2_x_ = prev1_x_;
    prev1_x_ = x;
    have_two_prev_ = true;
    return beat;
}

}  // namespace bw::signal
