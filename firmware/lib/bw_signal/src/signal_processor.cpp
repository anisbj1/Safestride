#include <bw/signal/signal_processor.h>

namespace bw::signal {

void SignalProcessor::feedImu(const ImuSample* samples, size_t count) {
    if (samples == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const ImuSample& s = samples[i];
        if (have_fed_imu_t_ && s.t_ms < last_fed_imu_t_ms_) {
            // Out-of-order timestamps cannot be staged (F02 D4): drop and
            // flag the frame — a Degraded window beats a wrong one.
            imu_.markFrameAnomaly();
            continue;
        }
        last_fed_imu_t_ms_ = s.t_ms;
        have_fed_imu_t_ = true;
        scheduler_.noteSample(s.t_ms);
        const Err pushed = imu_staging_.push(s);
        if (pushed != Err::Ok) {
            // Err::Full: staging headroom exceeded (caller ran >2.56 s
            // behind) — sample lost, window degrades, never crashes (F02 D4).
            imu_.markFrameAnomaly();
        }
    }
}

void SignalProcessor::feedPpg(const PpgSample* samples, size_t count) {
    if (samples == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const PpgSample& s = samples[i];
        if (have_fed_ppg_t_ && s.t_ms < last_fed_ppg_t_ms_) {
            ppg_stats_.anomaly = true;
            continue;
        }
        last_fed_ppg_t_ms_ = s.t_ms;
        have_fed_ppg_t_ = true;
        scheduler_.noteSample(s.t_ms);
        const Err pushed = ppg_staging_.push(s);
        if (pushed != Err::Ok) {
            ppg_stats_.anomaly = true;
        }
    }
}

bool SignalProcessor::step(TimeMs now_ms, SignalWindowOutput& out) {
    if (!scheduler_.shouldClose(now_ms)) {
        return false;
    }
    closeWindow(out);
    return true;
}

bool SignalProcessor::stepUntilIdle(TimeMs now_ms, SignalWindowOutput& latest) {
    bool closed_any = false;
    while (step(now_ms, latest)) {
        closed_any = true;
    }
    return closed_any;
}

size_t SignalProcessor::copyFilteredPpg(PpgFilteredPoint* out, size_t max) const {
    return filter_.copyDebug(out, max);
}

void SignalProcessor::drainImu(TimeMs window_end_ms) {
    // Parked sample first — it is the OLDEST not-yet-processed one (F02 D4).
    if (imu_carry_pending_ && imu_carry_.t_ms < window_end_ms) {
        imu_.feed(imu_carry_);
        imu_carry_pending_ = false;
    }
    ImuSample s{};
    while (!imu_carry_pending_ && imu_staging_.pop(s)) {
        if (s.t_ms < window_end_ms) {
            imu_.feed(s);
        } else {
            imu_carry_ = s;  // belongs to the next window; keep order
            imu_carry_pending_ = true;
        }
    }
}

void SignalProcessor::processPpgSample(const PpgSample& sample) {
    TimeMs gap_ms = 0;
    if (have_processed_ppg_t_ && sample.t_ms > last_processed_ppg_t_ms_) {
        gap_ms = sample.t_ms - last_processed_ppg_t_ms_;
    }
    const float filtered = filter_.push(sample.ir);
    (void)beat_detector_.push(filtered, sample.ir, sample.t_ms);
    if (profile_ == PpgProfile::Tester25Hz) {
        (void)tester_hr_.push(sample);
    }
    ppg_stats_.accumulate(sample, filtered, gap_ms);
    last_processed_ppg_t_ms_ = sample.t_ms;
    have_processed_ppg_t_ = true;
}

void SignalProcessor::drainPpg(TimeMs window_end_ms) {
    if (ppg_carry_pending_ && ppg_carry_.t_ms < window_end_ms) {
        processPpgSample(ppg_carry_);
        ppg_carry_pending_ = false;
    }
    PpgSample s{};
    while (!ppg_carry_pending_ && ppg_staging_.pop(s)) {
        if (s.t_ms < window_end_ms) {
            processPpgSample(s);
        } else {
            ppg_carry_ = s;
            ppg_carry_pending_ = true;
        }
    }
    // Tail gap: silence from the last processed sample to the window end
    // (a live stream ends within ~one sample period of the boundary).
    if (have_processed_ppg_t_ && window_end_ms > last_processed_ppg_t_ms_) {
        const TimeMs tail_ms = window_end_ms - last_processed_ppg_t_ms_;
        if (tail_ms > ppg_stats_.max_gap_ms) {
            ppg_stats_.max_gap_ms = tail_ms;
        }
    }
}

void SignalProcessor::closeWindow(SignalWindowOutput& out) {
    const TimeMs window_end_ms = scheduler_.windowEnd();

    // --- motion side ------------------------------------------------------
    drainImu(window_end_ms);
    MotionFeatures motion = imu_.close(window_end_ms);

    // Motion lookback over the HR window (F02 D6): push the current frame
    // (marked unknown when Insufficient — absence is not evidence of rest),
    // then flag when any KNOWN entry exceeded the artifact threshold.
    const MotionLookbackEntry current{motion.quality != DataQuality::Insufficient ? motion.enmo_g
                                                                                  : 0.0f,
                                      motion.quality != DataQuality::Insufficient};
    (void)motion_lookback_.push(current);

    bool motion_corrupted = false;
    for (size_t i = 0; i < motion_lookback_.size(); ++i) {
        const MotionLookbackEntry& e = *motion_lookback_.at(i);
        if (e.known && e.enmo_g > kMaxMotionForValidHrEnmoG) {
            motion_corrupted = true;
            break;
        }
    }

    // --- ppg side ---------------------------------------------------------
    drainPpg(window_end_ms);
    ppg_stats_.finalize(kMotionWindowMs, profile_ == PpgProfile::Tester25Hz
                                             ? kTesterPpgSamplePeriodMs
                                             : kPpgSamplePeriodMs);
    const HeartRateResult hr = HeartRateEstimator::estimate(beat_detector_.beats(), window_end_ms);
    PpgQualityResult quality = PpgQualityEstimator::evaluate(ppg_stats_, hr.consistency);
    if (profile_ == PpgProfile::Tester25Hz &&
        (!tester_hr_.contact() || ppg_stats_.dc_level < static_cast<float>(kTesterContactIr))) {
        // The reference's absolute contact gate also guards quality: dividing
        // ambient noise by a tiny DC must not turn "no finger" into Good SQI.
        quality.quality = DataQuality::Insufficient;
        quality.sqi = 0.0f;
    }
    if (motion_corrupted && quality.quality == DataQuality::Good) {
        quality.quality = DataQuality::Degraded;  // never upgrade, only degrade
    }

    // --- outputs ----------------------------------------------------------
    out.window_end_ms = window_end_ms;
    out.has_motion = true;
    out.motion = motion;

    PpgFeatures& ppg = out.ppg;
    ppg = PpgFeatures{};
    ppg.t_ms = window_end_ms;
    ppg.quality = quality.quality;
    // F02 semantic change (2026-10-15, developer-directed): hr_valid is the
    // beat-train PLAUSIBILITY flag only — HeartRateEstimator's own gates
    // (beats >= kMinBeatsForHr, median IBI within
    // [kPpgRefractoryMs, kIbiPlausibleMaxMs], miss-tolerant IBI consistency
    // >= kMinIbiConsistencyFraction — a ~2x gap is one missed beat, not
    // arrhythmia), all unchanged otherwise. The quality gate no longer
    // doubles as the validity gate: quality/sqi/perfusion/motion/saturation
    // stay exactly as strict and are carried alongside so consumers can
    // make their own trust decisions (F12 renders the plain number for any
    // valid rhythm since the 2026-10-15 evening reversal; F13 cataplexy
    // still requires non-Insufficient quality + a confidence floor). No
    // contract doc pins the old coupling (02 interfaces = field list only;
    // 00/01 never mention hr_valid) → no CR. hr_confidence still multiplies
    // sqi, so a low-quality window reports honest low confidence.
    // hr_valid LIFETIME policy (FIX loop 5, bench round 7): the estimator
    // answers "is THIS window's rhythm plausible?"; these rules decide how
    // long that answer lives on the display — instant and live like the
    // tester app, but never dishonest:
    //  1. BEAT GAP (precedence): the newest accepted beat is older than
    //     kIbiPlausibleMaxMs ⇒ the pulse stopped — clear NOW and release
    //     the hold (bounds a finger-removal ghost to ≤2.5 s + one window).
    //  2. Fresh valid ⇒ latch the hold; reset the implausible-run clock.
    //  3. Fresh invalid, hold latched for < kHrValidDropoutMs since the
    //     first failing window AND the full-window median still plausible
    //     ⇒ HOLD: keep the number (bpm keeps updating from the median —
    //     it self-corrects, developer decision) through short
    //     weak-perfusion/micro-motion dips instead of flickering to `--`.
    //  4. Otherwise ⇒ invalid; the hold never rescues arrhythmia (the
    //     estimator's recent-IBI gate rejected the rhythm) and never
    //     outlives an actual beat gap (rule 1 wins). The estimator's
    //     plausibility rules themselves are untouched — only the flag's
    //     lifetime lives here (F02 §8 mirrors both constants).
    const RingBuffer<TimeMs, kMaxBeatsInHrWindow>& beats = beat_detector_.beats();
    bool valid_now = hr.valid;
    const bool median_plausible = hr.median_ibi_ms >= static_cast<float>(kPpgRefractoryMs) &&
                                  hr.median_ibi_ms <= static_cast<float>(kIbiPlausibleMaxMs);
    if (beats.size() > 0 && window_end_ms - *beats.at(beats.size() - 1) > kIbiPlausibleMaxMs) {
        valid_now = false;
        hr_hold_active_ = false;  // pulse gone beats any hold
        hr_hold_started_ = false;
    } else if (hr.valid) {
        hr_hold_active_ = true;
        hr_hold_started_ = false;
    } else {
        if (hr_hold_active_ && !hr_hold_started_) {
            hr_hold_started_ = true;
            hr_hold_since_ms_ = window_end_ms;  // first window of the run
        }
        const bool within_hold =
            hr_hold_active_ && window_end_ms - hr_hold_since_ms_ < kHrValidDropoutMs;
        valid_now = within_hold && median_plausible;
        if (!valid_now) {
            hr_hold_active_ = false;
            hr_hold_started_ = false;
        }
    }
    ppg.hr_valid = valid_now;
    ppg.hr_bpm = valid_now
                     ? (hr.valid ? hr.hr_bpm : static_cast<float>(kMsPerMinute) / hr.median_ibi_ms)
                     : 0.0f;
    ppg.hr_confidence = ppg.hr_valid ? quality.sqi * hr.consistency : 0.0f;  // both in [0,1]
    if (profile_ == PpgProfile::Tester25Hz) {
        tester_hr_.updateTime(window_end_ms);
        // The reference average determines BPM/validity. The independent
        // bandpass/IBI quality channel is retained (never fabricate quality
        // merely because the reference detector found two peaks).
        ppg.hr_valid = tester_hr_.valid() && ppg_stats_.sample_count != 0 &&
                       ppg_stats_.max_gap_ms <= kMaxSampleGapMs;
        ppg.hr_bpm = ppg.hr_valid ? tester_hr_.bpm() : 0.0f;
        ppg.hr_confidence = ppg.hr_valid ? quality.sqi * hr.consistency : 0.0f;
    }
    ppg.perfusion_index = ppg_stats_.perfusion_index;
    ppg.ir_dc_level = ppg_stats_.dc_level;
    ppg.sqi = quality.sqi;
    ppg.motion_corrupted = motion_corrupted;
    // F02 D8: HRV deferred in v1 — validity flag carries "not available"
    // (02 §1); revisit at F29 with recorded captures.
    ppg.hrv_valid = false;
    ppg.rmssd_ms = 0.0f;
    // Bench diagnostics for [PPGWIN] (F02 FIX): captured before reset().
    out.ppg_sample_count = ppg_stats_.sample_count;
    out.ppg_beat_count = hr.beat_count;
    out.ppg_saturated_fraction = ppg_stats_.saturated_fraction;
    out.ppg_raw_ir_min = ppg_stats_.raw_ir_min;
    out.ppg_raw_ir_max = ppg_stats_.raw_ir_max;
    // FIX-2 [PPGPK]: in-band observability, mirrored from the detector's
    // own tallies (reset after capture so each window reports its own).
    out.ppg_filtered_std = ppg_stats_.filtered_std;
    out.ppg_strongest_peak = beat_detector_.obs().strongest_peak;
    out.ppg_peak_threshold = beat_detector_.obs().last_threshold;
    out.ppg_candidate_peaks = beat_detector_.obs().candidates;
    out.ppg_peaks_above_threshold = beat_detector_.obs().peaks_above;
    beat_detector_.resetObservability();  // per-window tallies (FIX-2)
    out.has_ppg = true;

    // --- advance ----------------------------------------------------------
    ppg_stats_.reset();  // anomaly/gap flags are per-frame; timestamps persist
    scheduler_.advance();
    // A parked sample that still sits beyond the NEW boundary keeps the
    // catch-up loop going on the next step() call (F02 D4).
    if (imu_carry_pending_ && imu_carry_.t_ms >= scheduler_.windowEnd()) {
        scheduler_.noteSample(imu_carry_.t_ms);
    }
    if (ppg_carry_pending_ && ppg_carry_.t_ms >= scheduler_.windowEnd()) {
        scheduler_.noteSample(ppg_carry_.t_ms);
    }
}

}  // namespace bw::signal
