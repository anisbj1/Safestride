#pragma once

// bw/signal/ppg_beat_detector.h — F02 §7: rhythm-constrained peak detection
// (FIX loop 4, bench round 7): refractory lockout + PI noise floor, with NO
// amplitude-adaptive detection state.
//
// Peak rule: x[n-1] is a peak when x[n-1] >= x[n-2] && x[n-1] > x[n]
// (one-sample delay; a flat top is detected at its last sample).
// Acceptance: a local maximum above the kMinPerfusionIndex x DC-EMA floor
// is a beat once kPpgRefractoryMs has passed since the last beat — the
// search window for the next beat is [kPpgRefractoryMs, kIbiPlausibleMaxMs]
// (the estimator's median gate rejects a rhythm outside it).
//
// FIX loop 4: the old adaptive term (kPeakAdaptiveFraction x amp_ema_, an
// EMA over ACCEPTED beats only) was a RATCHET under real 30–100 %
// beat-to-beat amplitude jitter: one strong beat lifted the bar above the
// next weak REAL beat → missed beat → IBI consistency collapse → display
// blanked until the 10 s starvation watchdog re-seeded the EMA — an on/off
// HR cycle under perfectly steady finger contact (bench round 7: 34/60
// valid windows on the jitter fixture). Both the adaptive state and its
// watchdog are DELETED: nothing in detection may depend on recent
// amplitudes, so the failure class cannot recur. Noise above the 0.1 % PI
// floor can still fabricate beats — that was already the accepted
// watchdog-floor trade; the honesty lives downstream (missed-beat-tolerant
// IBI consistency, SQI, motion flag).
//
// Tallest-in-window (dicrotic-notch safety): a floor-clearing peak inside
// the refractory lockout cannot be a beat, but it may be the window's
// tallest candidate — it is kept (value + own timestamp) and accepted when
// the lockout expires. Zero lookahead latency, fixed state, no heap.

#include <bw/containers/ring_buffer.h>
#include <bw/signal/signal_config.h>
#include <bw/types.h>

namespace bw::signal {

class PpgBeatDetector {
public:
    // Feeds one filtered sample; dc_raw is the raw IR of the SAME sample
    // (for the noise floor). Returns true when a beat is accepted.
    // The beat is stamped with t_ms: the one-sample detection delay (10 ms)
    // cancels in every inter-beat interval, so HR is unaffected [DERIVED].
    bool push(float x, uint32_t dc_raw, TimeMs t_ms);

    void reset();

    // Beat stamps of the last kHrWindowMs (ring overwrites the oldest,
    // which is history by design, F02 D6).
    const RingBuffer<TimeMs, kMaxBeatsInHrWindow>& beats() const { return beats_; }

    // FIX-2 bench observability ([PPGPK] line): per-window tallies counted
    // inside push() from the SAME peak rule and the SAME threshold that
    // drive detection — no second detector copy. candidates counts local
    // maxima before any gate; peaks_above counts candidates taller than
    // the threshold (also before the refractory gate), so b << peaks_above
    // would isolate the refractory while peaks_above == 0 isolates the
    // threshold/noise floor. strongest_peak is the tallest candidate seen
    // since the last resetObservability(); last_threshold is the threshold
    // in force for the most recent sample (0 right after a reset — no
    // sample has asked for a threshold yet). The processor resets these at
    // every closed window; detection state is untouched.
    struct Observability {
        uint16_t candidates = 0;
        uint16_t peaks_above = 0;
        float strongest_peak = 0.0f;
        float last_threshold = 0.0f;
    };
    const Observability& obs() const { return obs_; }
    void resetObservability() { obs_ = Observability{}; }

private:
    RingBuffer<TimeMs, kMaxBeatsInHrWindow> beats_;
    float prev2_x_ = 0.0f;  // x[n-2]
    float prev1_x_ = 0.0f;  // x[n-1]
    bool have_two_prev_ = false;
    float dc_ema_ = 0.0f;  // DC (raw IR) estimate for the noise floor
    bool have_dc_ = false;
    // Tallest floor-clearing candidate inside the current refractory lockout
    // (FIX loop 4), accepted at the lockout edge with its own timestamp.
    bool have_locked_peak_ = false;
    TimeMs locked_t_ms_ = 0;
    float locked_amp_ = 0.0f;
    TimeMs last_beat_t_ms_ = 0;
    bool have_last_beat_ = false;
    Observability obs_;
};

}  // namespace bw::signal
