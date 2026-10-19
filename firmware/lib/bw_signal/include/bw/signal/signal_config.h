#pragma once

// bw/signal/signal_config.h — F02 §8 (named constants, every value tagged and
// mirrored in the spec table). Compile-time configuration for now (F02 D9):
// when F18 settings land, the EXPERIMENTAL values move into a SignalConfig
// struct + [USER] keys via a change request if a contract type is needed.

#include <bw/types.h>

namespace bw::signal {

// ---- windowing -----------------------------------------------------------

// [EXPERIMENTAL] 1 s gives stable ENMO with 50 samples while keeping decision
// latency low; tune with F28 captures (F02 §8).
constexpr TimeMs kMotionWindowMs = 1000;

// [DERIVED] 1000 ms is the common 1 Hz feature cadence (01 §3.2/§3.3).
constexpr uint32_t kMsPerSecond = 1000;

// [DERIVED] bpm = 60 000 ms / IBI — unit conversion for heart rate.
constexpr uint32_t kMsPerMinute = 60u * kMsPerSecond;

// [DERIVED] 180 / pi — unit conversion for tilt output in degrees.
constexpr float kRadiansToDegrees = 57.2957795f;

// [HW] MPU6050 outputs are multiples of local g (F01 configures g/LSB).
constexpr float kStandardGravityG = 1.0f;

// [DERIVED] 50 Hz — Nyquist margin for <20 Hz arm-motion energy (01 §3.3).
constexpr uint32_t kImuSamplePeriodMs = 20;

// [DERIVED] 100 Hz — 200 bpm = 3.3 Hz, waveform harmonics to ~10 Hz (01 §3.3).
constexpr uint32_t kPpgSamplePeriodMs = 10;

// Explicit processing profiles retain the 100 Hz replay pipeline while the
// breadboard uses the developer's verified MAX30102 tester configuration.
enum class PpgProfile : uint8_t { Filtered100Hz, Tester25Hz };
// [DERIVED] MAX30102: 100 conversions/s / 4 averaged conversions = 25 FIFO/s.
constexpr uint32_t kTesterPpgSamplePeriodMs = 40;
// [EXPERIMENTAL] Same hardware reference: below 50000 IR counts is no contact.
constexpr uint32_t kTesterContactIr = 50000;
// [EXPERIMENTAL] Tester smoothing: 60% previous derivative, 40% new difference.
constexpr float kTesterDerivativeAlpha = 0.4f;
// [EXPERIMENTAL] Measured tester pulse slopes arm above +80, crest below -20.
constexpr float kTesterDerivativeArm = 80.0f;
constexpr float kTesterDerivativeFall = -20.0f;
// [EXPERIMENTAL] Tester rejects close peaks (<450 ms) and reanchors after 2 s.
constexpr TimeMs kTesterMinIbiMs = 450;
constexpr TimeMs kTesterMaxIbiMs = 2000;
// [EXPERIMENTAL] Tester accepts 40..200 BPM in addition to the IBI bounds.
constexpr float kTesterMinBpm = 40.0f;
constexpr float kTesterMaxBpm = 200.0f;
// [DERIVED] Four accepted instantaneous BPM values, matching the reference.
constexpr size_t kTesterBpmHistorySize = 4;
// [EXPERIMENTAL] Reference clears BPM/history after >3 s without a valid beat.
constexpr TimeMs kTesterBpmStaleMs = 3000;

// [EXPERIMENTAL] Gap larger than 2.5 PPG periods (or 12.5 IMU periods) marks
// the window Degraded; a stalled stream closes kMaxSampleGapMs after the
// window end (F02 §8, D4).
constexpr TimeMs kMaxSampleGapMs = 250;

// ---- motion / IMU ---------------------------------------------------------

// [EXPERIMENTAL] Below typical sensor noise + micro-motion of a resting
// user; tune with F28 captures (F02 §8).
constexpr float kStillnessEnmoThresholdG = 0.02f;

// [EXPERIMENTAL] Hysteresis exit = 2.5x the enter threshold so sensor noise
// around 0.02 g cannot flap the stillness state (F02 D5, named-constants:
// thresholds come in pairs where hysteresis is needed).
constexpr float kStillnessExitEnmoThresholdG = 0.05f;

// [EXPERIMENTAL] Gravity low-pass time constant: slow enough that arm swing
// stays out of the gravity vector, fast enough to track posture change in ~2 s.
constexpr TimeMs kGravityTimeConstantMs = 2000;

// [EXPERIMENTAL] Initial guess; tune with recorded data (F29).
// Why: one gravity update per IMU sample at the nominal rate; alpha =
// dt / (tau + dt) keeps the filter independent of loop timing (F02 D5).
constexpr float kGravityLpAlpha = static_cast<float>(kImuSamplePeriodMs) /
                                  static_cast<float>(kGravityTimeConstantMs + kImuSamplePeriodMs);

// [DERIVED] 1/4 s window => 0.25 Hz resolution, enough to separate walking
// cadence bands; autocorrelation of 200 samples x <=98 lags ~ 20 k MACs once
// per second (F02 D5). Cheaper than an FFT for one band (F02 D1).
constexpr TimeMs kDominantFreqWindowMs = 4000;

// [DERIVED] 0.5 Hz: below it the signal is drift, not activity.
constexpr float kDominantFreqMinHz = 0.5f;

// [DERIVED] 20 Hz: arm-motion energy is below ~20 Hz (01 §3.3);
// Nyquist at 50 Hz is 25 Hz.
constexpr float kDominantFreqMaxHz = 20.0f;

// [EXPERIMENTAL] Normalized autocorrelation peak below ~0.3 means there is no
// real periodicity; report dominant_freq_hz = 0 ("0 if none", 02 §4.2).
constexpr float kMinAutocorrPeakStrength = 0.3f;

// [EXPERIMENTAL] Relative margin a longer lag must beat the current best by
// before we prefer it. A periodic signal produces near-equal autocorrelation
// peaks at every multiple of the period (lag T, 2T, ...); without a margin the
// numerically largest of those ties wins arbitrarily and reports a
// subharmonic (measured: 1.5 Hz tone read as 0.75 Hz at lag 2T). 2% is well
// above float noise on a ~50-sample window and well below the ~40% gap between
// a true peak and its neighbors, so the first (shortest = true) peak wins.
constexpr float kAutocorrTieMargin = 0.02f;

// [DERIVED] Full kDominantFreqWindowMs of samples at the nominal rate is the
// least from which a periodicity claim is meaningful; until then report 0.
constexpr uint32_t kMinSamplesForDominantFreq =
    static_cast<uint32_t>(kDominantFreqWindowMs / kImuSamplePeriodMs);

// [DERIVED] 2.56 s at 50 Hz: two 1 s task cadences of headroom before feed()
// starts dropping (Overflow) and windows degrade — never crash (F02 D4).
constexpr size_t kImuStagingCapacity = 128;

// [DERIVED] Minimum capacity of the AppContext F01->F02 input queue (D10):
// one full 1 s consumer period (50 samples at 50 Hz) + 28 % jitter margin.
// The actual N is spelled by the integrator in app.h (>= this); the future
// wire_f02 static_asserts against it.
constexpr size_t kImuInputQueueMinCapacity = 64;

// [EXPERIMENTAL] Above this mean window ENMO motion artifacts dominate
// the PPG; tune against recorded data (F02 §8).
constexpr float kMaxMotionForValidHrEnmoG = 0.15f;

// ---- PPG filter -----------------------------------------------------------

// [DERIVED] 30-240 bpm covers physiological HR with margin (F02 §8).
// Coefficients: tools/design_ppg_filter.py (Butterworth, Q = 1/sqrt(2)).
constexpr float kPpgBandpassLowHz = 0.5f;
constexpr float kPpgBandpassHighHz = 4.0f;

// [DERIVED] DC-blocker corner far below the 0.5-4 Hz band: removes baseline
// wander from contact/pressure changes with no passband impact (F02 D7).
constexpr float kPpgDcBlockerHz = 0.05f;

// [DERIVED] Butterworth 2nd-order section quality factor Q = 1/sqrt(2)
// (maximally flat passband); consumed by the design script.
constexpr float kBiquadButterworthQ = 0.70710678f;

// ---- PPG beat detection / heart rate --------------------------------------

// [DERIVED] 200 bpm => 300 ms between beats; rejects dicrotic-notch
// double-detection (F02 §8). Also the inner edge of the search window for
// the next beat (FIX loop 4: [kPpgRefractoryMs, kIbiPlausibleMaxMs]).
constexpr TimeMs kPpgRefractoryMs = 300;

// [DERIVED] Noise-floor = kMinPerfusionIndex x DC EMA: peaks below the
// minimum trustworthy perfusion (0.1 % AC/DC) can never yield valid HR, so
// reusing the same ratio keeps one number instead of two (F02 D6).
// DC EMA: tau = 1/(alpha * fs) ~ 1 s at 100 Hz — slower than any beat
// (>= 300 ms) so the floor does not chase the pulse.
constexpr float kPpgDcEmaAlpha = 0.01f;

// [EXPERIMENTAL] Below the pulsatile component of a resting user is 0.1 % of
// DC; below it AC is indistinguishable from noise for this sensor (F02 §8).
// The contract stores perfusion as the AC/DC fraction, so 0.1 % = 0.001.
constexpr float kMinPerfusionIndex = 0.001f;

// [EXPERIMENTAL] Full-quality perfusion reference, RECALIBRATED 2026-10-14
// against the first real-sensor data (developer's tester ground truth, same
// wiring: working pulse = AC 500–2000 counts on DC ~150 000 → PI band
// ≈ 0.3–1.3 %; our estimator computes PI = ac_rms/dc, so a sine of peak A
// gives PI = A/(√2·dc)). 0.4 % = the rms-PI of a weak-but-clear pulse
// (peak ≈ 600 counts at DC 163 000 — the bottom of that measured band).
// The previous 1 % demanded AC rms 1630 counts (≈4600 counts p-p) for
// "full perfusion" — a level this sensor never produced on the bench —
// and capped sqi at 0.18 for a pulse the developer's app reads
// comfortably (H3, loop-2b audit). Tune further with F28/F29.
constexpr float kSqiFullPerfusionRef = 0.004f;

// [EXPERIMENTAL] A couple of clipped samples per window are ADC glitches;
// >= 2 % means the pulse left the sensor's linear range (F02 D6).
constexpr float kMaxSaturatedFraction = 0.02f;

// [EXPERIMENTAL] SQI below this (product of perfusion x timing x saturation
// factors) means the window's content is unusable -> Insufficient (F02 D6,
// §9: noise-only and saturated input must land there).
constexpr float kMinSqiForValidHr = 0.4f;

// [EXPERIMENTAL] Half a window of real samples is the least from which
// mean/std are meaningful; below it the window is Insufficient (F02 D6).
constexpr float kMinWindowCoverageFrac = 0.5f;

// [EXPERIMENTAL] Initial guess; tune with recorded data (F29).
// Why: 10 s >= 8 s holds enough beats at 60 bpm for a stable median IBI
// (F02 §8).
constexpr TimeMs kHrWindowMs = 10000;

// [DERIVED] HR is gated on motion over the whole kHrWindowMs lookback, not
// just the emitted frame: an artifact corrupts beats for seconds (F02 D6).
constexpr size_t kHrMotionLookbackWindows = static_cast<size_t>(kHrWindowMs / kMotionWindowMs);

// [DERIVED] 10 s x 200 bpm / 60 = 33 beats + headroom; the ring overwrites
// the oldest stamp (history, not a data-loss error).
constexpr size_t kMaxBeatsInHrWindow = 40;

// [DERIVED] 10 s at the 0.5 Hz (30 bpm) band edge gives 5 beats; 4 tolerates
// one rejected artifact beat (F02 D6).
constexpr uint32_t kMinBeatsForHr = 4;

// [DERIVED] 30 bpm = 2000 ms is the band edge; 2500 ms allows one borderline
// beat. Longer than this means a dropout, not a beat pair. The lower bound is
// kPpgRefractoryMs (300 ms = 200 bpm).
constexpr TimeMs kIbiPlausibleMaxMs = 2500;

// [EXPERIMENTAL] Sinus arrhythmia is ~10 % of the IBI; missed/extra beats
// deviate >= 50 % — a 20 % band around the median separates them (F02 D6).
constexpr float kIbiMedianToleranceFrac = 0.2f;

// [EXPERIMENTAL] Below ~60 % of IBIs inside the tolerance band the timing is
// erratic: HR from it is more likely wrong than right (F02 D6).
constexpr float kMinIbiConsistencyFraction = 0.6f;

// [DERIVED] Missed-beat multiple (FIX loop 4, bench round 7): an IBI of
// ~2x the median is ONE MISSED beat (its amplitude dipped below the
// detection floor), not arrhythmia — it now counts toward
// kMinIbiConsistencyFraction alongside 1x IBIs. A 3x+ gap stays outside
// both bands and remains a rhythm break (pause, artifact, genuine
// arrhythmia). Self-limiting by construction: the median is computed from
// the raw IBIs, so ≥ ~50 % misses skew the median into no band and the
// train honestly fails (documented in F02 §9).
constexpr float kIbiMissedBeatMultiple = 2.0f;

// [DERIVED] Validity-gate consistency horizon (FIX loop 5, bench round 7):
// consistency_miss_tolerant (the `valid` gate) is computed over the NEWEST
// kHrRecentIbiCount IBIs, not the whole kHrWindowMs lookback. Why 4: it is
// kMinBeatsForHr expressed in IBIs (4 beats → 3 IBIs exist at the first
// possible verdict, so the horizon never delays first display), ≈3.4 s at
// 70 bpm — a weak-perfusion / micro-motion stretch ages out of the gate
// within ~2 windows of its end instead of lingering for the rest of the
// 10 s lookback (round 7: a ~4 s weak stretch kept hr_valid=false for 14 s
// under steady contact), and one artifact IBI among 4 still passes
// (3/4 = 0.75 ≥ kMinIbiConsistencyFraction) while two consecutive bad ones
// fail (0.5). The MEDIAN and the strict `consistency` (SQI, hr_confidence)
// stay full-window — reliability of the whole window is exactly what those
// must keep honest (F02 §8).
constexpr uint32_t kHrRecentIbiCount = 4;

// [DERIVED] Hold timeout for hr_valid (FIX loop 5, bench round 7): once a
// window was plausibly valid, consecutive fresh-invalid windows shorter
// than this KEEP the number on the display (bpm keeps updating from the
// median) instead of flickering to `--` — the developer's 3–5 s band, upper
// edge: long enough to ride a 4 s weak stretch plus the ~1 window its bad
// IBIs need to age out of kHrRecentIbiCount, short enough that a genuine
// rhythm change shows within ~5 s. The hold NEVER outlives a beat gap: no
// accepted beat for kIbiPlausibleMaxMs clears validity and the hold
// immediately (pulse stopped), so a finger-lift ghosts ≤2.5 s + one window
// regardless of this timeout.
constexpr TimeMs kHrValidDropoutMs = 5000;

// ---- output staging -------------------------------------------------------

// [DERIVED] 2.56 s at 100 Hz: same two-cadence headroom as IMU staging
// (F02 D4).
constexpr size_t kPpgStagingCapacity = 256;

// [DERIVED] Minimum capacity of the AppContext PPG input queue (D10): one full
// 1 s consumer period (100 samples at 100 Hz) + 28 % jitter margin. The actual
// N is spelled by the integrator in app.h (>= this).
constexpr size_t kPpgInputQueueMinCapacity = 128;

// [DERIVED] 5 s of filtered IR for the F28 debug tap (F02 §3); larger
// replays use F28's own raw capture path. 500 x 8 B = 4 KB.
constexpr size_t kPpgFilteredDebugCapacity = 500;

}  // namespace bw::signal
