#pragma once

// bw/diag/quality_hysteresis.h — F23 D7: per-source commit filter between
// the assessor's instantaneous judgment and the published DataQuality.
//
// Shape (D7, integrator-pinned G1): fail-safe asymmetry —
//  - Insufficient enters IMMEDIATELY (one bad window drops the source;
//    honesty about bad data is never delayed, spec §10);
//  - Degraded enters after DiagConfig::degraded_enter_windows consecutive
//    Degraded windows (glitch filter);
//  - any recovery to Good needs DiagConfig::good_recover_windows
//    consecutive Good windows (slow recovery, gating errs toward "no
//    reliable data").
//
// Pure logic: step-style API, time-free (windows are ticks), no FreeRTOS/
// Arduino calls (01 §1 rule 5).

#include <bw/diag/diag_config.h>
#include <bw/quality.h>

namespace bw::diag {

class QualityHysteresis {
public:
    explicit QualityHysteresis(const DiagConfig& config = DiagConfig{}) : config_(config) {}

    // One tick: fold in the instantaneous judgment, return the committed
    // quality. Boot state is Insufficient (fail-safe §10): the first Good
    // output needs a full good_recover_windows of healthy input.
    DataQuality update(DataQuality instantaneous);

    DataQuality current() const { return current_; }

    // Back to the boot state (fail-safe restart).
    void reset();

private:
    // True when `to` is a better level than `from` (enum order: Good=0 ..
    // Insufficient=2, so smaller value = better).
    static bool isBetter(DataQuality to, DataQuality from);

    DiagConfig config_;
    DataQuality current_ = DataQuality::Insufficient;  // fail-safe start (§10)
    DataQuality run_level_ = DataQuality::Good;        // level counted by run_count_
    uint8_t run_count_ = 0;                            // consecutive ticks of run_level_
};

}  // namespace bw::diag
