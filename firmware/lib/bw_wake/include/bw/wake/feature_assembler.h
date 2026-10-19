#pragma once

// bw/wake/feature_assembler.h — F06 §7: assembles the multimodal feature
// set (motion, PPG, activity, time-of-day, recent history, baseline-relative
// values) and folds 1 s windows into epochs (F06 D2).
//
// Responsibilities per the spec's own sentence: F06 "assembles the multimodal
// feature vector ... and produces AlertnessProbabilities". This class owns the
// assembly half — gating, epoching, history — and exposes the schema-driven
// vector builder that the model path (and, later, F05's generated header)
// consumes.

#include <bw/clock.h>
#include <bw/containers/ring_buffer.h>
#include <bw/model.h>
#include <bw/providers.h>
#include <bw/result.h>

#include <bw/wake/alertness_schema.h>
#include <bw/wake/wake_config.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

// kHistoryEpochs = 10 (F06 §8) of completed epochs, oldest at at(0).
using EpochHistory = RingBuffer<EpochSummary, kHistoryEpochs>;

// --- history-derived evidence ----------------------------------------------
// Free functions on purpose: the heuristic rules, the schema vector and any
// future estimator must agree on what "duration" and "drop" mean, and one
// definition is the only way to guarantee that (00 §7: one responsibility
// each, no duplicated logic).

// Consecutive low-motion epochs including `epoch`, capped by the ring.
// Returns 0 as soon as `epoch` itself is not low-motion, so a busy epoch
// always resets duration evidence. Epochs that missed the IMU quorum
// (imu_usable == false) are streak-breakers too: a sparse epoch must never
// extend a run whose evidence it cannot back (F06 §5).
uint16_t lowMotionEpochStreak(const EpochSummary& epoch, const EpochHistory& history,
                              const WakeConfig& cfg);

// Share of the previous epoch's mean ENMO that `epoch` lost, 0..1.
// 0 when there is no previous epoch, when it was motionless (nothing to
// measure a drop against — absence is 0 evidence, not a fabricated spike),
// or when it is not comparable (imu_usable == false): a data gap never
// becomes a trend reference.
float enmoDropRatio(const EpochSummary& epoch, const EpochHistory& history);

// The epoch before history()'s newest, or nullptr on the first epoch —
// and also nullptr when that epoch missed the IMU quorum (imu_usable ==
// false), because a sparse epoch is not a comparable trend reference
// (HR-trend and ENMO-drop both must not bridge a gap). One definition, so
// every caller gets the same "no reference" answer.
//
// Invariant: the assembler pushes every epoch it commits, so history()'s
// newest slot IS the current epoch — "previous" therefore starts one slot
// older. Every helper in this header relies on that, and the bw_wake tests
// pin it.
const EpochSummary* previousEpoch(const EpochHistory& history);

// Single definition of "the HR term may speak": a personal baseline to
// compare against AND enough valid beats in the epoch for the mean to be a
// measurement (kMinHrValidRatio). Missing either way = no HR evidence, which
// is not the same as "HR is normal".
bool hrEvidenceUsable(const EpochSummary& epoch, const WakeConfig& cfg);

// Schema-driven assembly: epoch aggregates (+ history) -> normalized
// FeatureVector. Normalization is the single formula of FEATURE_SCHEMA §4
// (x - mean) / std with "none" rewritten to identity; feature order is
// normative (kAlertnessFeatureNames).
//
// Fails only when the epoch carries no windows (Err::NotReady) — a missing
// input is never filled in (F06 §5).
Result<void> buildFeatureVector(const WakeConfig& cfg, const EpochSummary& epoch,
                                const EpochHistory& history, TimeMs t_ms, FeatureVector& out);

class FeatureAssembler {
public:
    // `clock` and `baseline` may be nullptr (AppContext carries no device
    // clock before M1 — app.h — and F13's baseline is explicitly optional,
    // 02 §5): absence is handled, never guessed at.
    FeatureAssembler(const WakeConfig& cfg, const IClock* clock, const IBaselineProvider* baseline);

    // Folds one 1 s window into the current epoch. Returns true when the
    // sample crossed an epoch boundary: `out` then holds a just-committed
    // summary, which is also history()'s newest element. Returns false when
    // the window simply accumulated.
    //
    // A timestamp that moves backwards (reboot, 01 §4) commits what was
    // accumulated and restarts — it never rewrites the past.
    bool step(const WakeSample& sample, EpochSummary& out);

    const EpochHistory& history() const { return history_; }

    // Drops epoch and history state (tests, and the composition root after a
    // pipeline restart).
    void reset();

private:
    // Per-epoch accumulators (F06 D4): fixed cost regardless of how many
    // windows arrive, so RAM does not scale with the epoch length.
    struct Accumulators {
        TimeMs t_ms;
        DataQuality worst_imu;  // worst quality seen in the epoch
        DataQuality worst_ppg;
        uint16_t windows;      // all windows folded in
        uint16_t imu_windows;  // ... of which usable
        uint16_t ppg_windows;
        uint16_t hr_windows;        // ... with a valid HR
        uint16_t activity_windows;  // ... with a usable activity estimate
        uint16_t low_motion_windows;
        uint16_t activity_hist[kActivityContextCount];
        float enmo_sum_g;
        float mag_std_sum_g;
        float jerk_sum_g_per_s;
        float stillness_max_s;
        float hr_sum_bpm;
        float activity_conf_sum;
    };

    void accumulate(const WakeSample& sample);
    void commit(EpochSummary& out);
    void clearAccumulators();

    WakeConfig cfg_;
    const IClock* clock_;
    const IBaselineProvider* baseline_;
    EpochHistory history_;
    Accumulators acc_;
    TimeMs epoch_index_;  // t_ms / epoch_length_ms of the open epoch
    bool acc_active_;
};

}  // namespace bw::wake
