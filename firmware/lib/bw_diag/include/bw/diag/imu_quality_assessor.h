#pragma once

// bw/diag/imu_quality_assessor.h — F23 D2/D6: instantaneous IMU quality
// judgment for one 1 Hz tick, plus the ImuStuckSensor diagnosis.
//
// Rules (spec §15.3, §15.4, plan D6 — feature/status level is the M2
// production path, raw windows are host-tested now and fed at M5):
//  - status missing / Failed / Uninitialized  ⇒ Insufficient, NO F23 edge
//    (F01 owns ImuAbsent/ImuReadFailed — F23 downgrades quality only,
//    never double-reports);
//  - features missing or older than kFeatureStalenessMs ⇒ Insufficient,
//    no fault (staleness is F26 TaskStalled territory at M5);
//  - bit-identical 1 Hz MotionFeatures frames (all fields except t_ms)
//    for ≥ kStuckFeatureWindows windows ⇒ ImuStuckSensor (Error) +
//    Insufficient — deterministic pipeline ⇒ identical frames mean
//    identical raw input (D6a);
//  - bit-identical raw samples for ≥ kStuckSamples samples ⇒
//    ImuStuckSensor + Insufficient (D6b; not evaluated when the window is
//    absent — never a guess, and an absent window also ENDS an open raw
//    episode: latch + run reset, so a stale latch never pins the source
//    and the next window must earn its own run). Evaluated whenever a
//    window is present, even when the feature stream is stale: raw
//    evidence is direct and does not depend on the feature pipeline
//    being alive;
//  - healthy ⇒ worst-of(status Degraded hint, F02's own quality hint).
//
// Edges are condition-transitions (latched while the condition holds,
// released when it clears); Diagnostics rate-limits reports (D9).
//
// Pure logic: no FreeRTOS/Arduino calls (01 §1 rule 5).

#include <cstddef>

#include <bw/diag/diag_config.h>
#include <bw/diag/diag_types.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/quality.h>
#include <bw/types.h>

namespace bw::diag {

struct ImuAssessOutput {
    DataQuality instantaneous = DataQuality::Insufficient;
    FaultEdge edge{};  // code None when no new transition this tick
};

class ImuQualityAssessor {
public:
    explicit ImuQualityAssessor(const DiagConfig& config = DiagConfig{}) : config_(config) {}

    // One tick. `features`/`raw` are nullptr when absent; staleness of the
    // feature frame is computed from now_ms - features->t_ms inside.
    ImuAssessOutput step(TimeMs now_ms, bool has_status, SensorStatus status,
                         const MotionFeatures* features, const ImuSample* raw, size_t raw_n);

    // Back to first-call state (tests, future F26 reinit hooks).
    void reset();

private:
    // Field-wise equality except t_ms: identical frames means identical
    // sensor content, while t_ms changes every window by design.
    static bool sameFrame(const MotionFeatures& a, const MotionFeatures& b);
    static bool sameSample(const ImuSample& a, const ImuSample& b);

    DiagConfig config_;

    // 1 Hz frame-run state (D6a). latched_ = the condition currently
    // holds; edge_emitted_ = this episode already produced its edge.
    MotionFeatures prev_frame_{};
    bool has_prev_frame_ = false;
    uint8_t frame_run_ = 0;
    bool frames_latched_ = false;
    bool frames_edge_emitted_ = false;

    // Raw run state (D6b), same split.
    ImuSample prev_raw_{};
    bool has_prev_raw_ = false;
    uint32_t raw_run_ = 0;
    bool raw_latched_ = false;
    bool raw_edge_emitted_ = false;
};

}  // namespace bw::diag
