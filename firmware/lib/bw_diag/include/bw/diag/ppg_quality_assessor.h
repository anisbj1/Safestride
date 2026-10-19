#pragma once

// bw/diag/ppg_quality_assessor.h — F23 D2/D6: instantaneous PPG quality
// judgment for one 1 Hz tick plus the PpgFlatline / PpgSaturated /
// PpgPoorContact diagnoses.
//
// Rules (spec §15.3, §15.4, plan D6):
//  - status missing / Failed / Uninitialized ⇒ Insufficient, NO F23 edge
//    (F01 owns PpgAbsent / PpgReadFailed — quality downgrade only);
//  - status Ok but no fresh features ⇒ Insufficient (fail-safe: "no data
//    is no data", never Good by absence);
//  - identical ir_dc_level across kStuckFeatureWindows windows with
//    perfusion below kPoorContactPerfusionIndex ⇒ PpgFlatline (Error) +
//    Insufficient (a constant DC with no pulsation is dead input);
//  - status Ok ⇒ no hint; status Degraded ⇒ at least Degraded (worst-of
//    with the F02 hint — same driver-hint mapping as the IMU assessor,
//    §15.4);
//  - perfusion_index < kPoorContactPerfusionIndex while the stream is
//    present (and not flat-line) ⇒ PpgPoorContact (Warning) + Degraded;
//  - raw window: saturated fraction ≥ kSaturatedWindowFraction ⇒
//    PpgSaturated (Error); identical-IR run ≥ kPpgFlatlineRawSamples ⇒
//    PpgFlatline (Error); both + Insufficient. Window absent ⇒ "not
//    evaluated", never a guess (D6; M2 host-tested, M5 feed).
//  - motion_corrupted ⇒ at least Degraded (F02's cross-modal hint).
//  - healthy ⇒ worst-of(F02 quality hint, rules above).
//
// Pure logic (01 §1 rule 5). Edges are condition-transitions, latched
// while the condition holds (D9 rate-limiting happens in Diagnostics).

#include <cstddef>

#include <bw/diag/diag_config.h>
#include <bw/diag/diag_types.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/quality.h>
#include <bw/types.h>

namespace bw::diag {

struct PpgAssessOutput {
    DataQuality instantaneous = DataQuality::Insufficient;
    FaultEdge edge{};  // code None when no new transition this tick
};

// Result of the standalone raw-window evaluator (§15.1): also the "not
// evaluated" answer when the window is empty.
struct PpgRawEval {
    DataQuality instantaneous = DataQuality::Insufficient;
    FaultEdge edge{};
};

class PpgQualityAssessor {
public:
    explicit PpgQualityAssessor(const DiagConfig& config = DiagConfig{}) : config_(config) {}

    // One tick; `features`/`raw` are nullptr when absent, staleness of the
    // feature frame is computed from now_ms - features->t_ms inside.
    PpgAssessOutput step(TimeMs now_ms, bool has_status, SensorStatus status,
                         const PpgFeatures* features, const PpgSample* raw, size_t raw_n);

    // Saturation / flat-line check over one raw window (§15.1, plan D6).
    // Empty window ⇒ {Insufficient, no edge} = "not evaluated".
    PpgRawEval evaluatePpgRawWindow(const PpgSample* samples, size_t n) const;

    void reset();

private:
    static bool sameDc(const PpgFeatures& a, const PpgFeatures& b);

    DiagConfig config_;

    // 1 Hz identical-DC run (flat-line via features).
    PpgFeatures prev_frame_{};
    bool has_prev_frame_ = false;
    uint8_t dc_run_ = 0;

    // Continuous raw IR run spans the bounded 1 s tee windows (which cannot
    // individually reach the 250-sample threshold). Absent windows reset it.
    uint32_t raw_ir_ = 0;
    uint32_t raw_run_ = 0;
    bool has_raw_ir_ = false;

    // Condition/edge latches per code (transition once per episode).
    bool flatline_latched_ = false;
    bool flatline_edge_emitted_ = false;
    bool poor_contact_latched_ = false;
    bool poor_contact_edge_emitted_ = false;
    bool saturated_latched_ = false;
    bool saturated_edge_emitted_ = false;
};

}  // namespace bw::diag
