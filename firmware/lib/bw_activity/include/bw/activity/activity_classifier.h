#pragma once

// bw/activity/activity_classifier.h — F03 §7: rule-based activity context
// (documented decision table R1–R9 in the F03 spec §6) from one
// MotionFeatures window, at the input's 1 Hz cadence.
//
// Pure logic: `ActivityEstimate step(const MotionFeatures&)`, no platform
// calls, no heap — time arrives inside the input (embedded conventions).
// Low movement never implies sleep (F03 §5): this class never outputs
// sleep/drowsiness and reports Unknown whenever the table cannot classify
// or the data is Insufficient (spec §10).
//
// D4 smoothing: context changes must hold for kMinDwellWindows consecutive
// windows, except quality == Insufficient which commits Unknown immediately.
// D5 confidence: base[class] × quality factor, floored at
// kMinConfidenceToReport; Unknown always carries confidence 0.0.

#include <bw/activity/activity_config.h>
#include <bw/activity/cadence_detector.h>
#include <bw/context.h>
#include <bw/features.h>

namespace bw::activity {

class ActivityClassifier {
public:
    ActivityClassifier() = default;
    explicit ActivityClassifier(const ActivityConfig& config) : config_(config), cadence_(config) {}

    // Feeds one 1 Hz MotionFeatures window and returns the smoothed estimate
    // stamped with the input's t_ms. Robust to any input: Insufficient,
    // backwards timestamps and large time jumps are handled (spec §10).
    ActivityEstimate step(const MotionFeatures& features);

    // Current smoothed context (for tests/diagnostics; step() is the API).
    ActivityContext context() const { return current_; }

private:
    // One evaluation of the R1–R9 decision table (F03 spec §6) before dwell
    // smoothing. Never called with quality == Insufficient (step() returns
    // the R1 commit first).
    struct RawClassification {
        ActivityContext context;
        float confidence;
    };
    RawClassification classify(const MotionFeatures& features, CadenceState cadence) const;

    // Resets dwell evidence when t_ms is backwards or jumps by more than
    // max_feature_step_gap_ms (windows were lost → evidence not continuous).
    void noteTimeAndMaybeResetDwell(TimeMs t_ms);

    ActivityEstimate estimate(TimeMs t_ms) const {
        return ActivityEstimate{t_ms, current_, current_confidence_};
    }

    ActivityConfig config_{};
    CadenceDetector cadence_{};

    // Dwell smoothing state.
    ActivityContext current_ = ActivityContext::Unknown;
    float current_confidence_ = 0.0f;
    ActivityContext candidate_ = ActivityContext::Unknown;
    float candidate_confidence_ = 0.0f;
    uint8_t dwell_windows_ = 0;
    TimeMs prev_t_ms_ = 0;
    bool have_prev_t_ = false;
};

}  // namespace bw::activity
