#pragma once

// bw/cataplexy/candidate_detector.h — F09 §7 EventCandidateDetector, spec §6 D1/D2.
//
// Phase 2 rule trigger: an abrupt loss of arm motion, judged over the last
// kAbruptDropWindowMs of feature windows and scored to a confidence that
// confounders (fall, posture change, short burst) and data quality can lower.
// Output is always a *suspected* event with confidence and reason codes —
// never a diagnosis (00 §1, spec §5).
//
// Pure logic: Output step(const Input&), time arrives as TimeMs, no FreeRTOS /
// Arduino calls (01 §1 rule 5), so the whole rule runs in `pio test -e native`.

#include <cstddef>
#include <cstdint>

#include <bw/cataplexy/cataplexy_config.h>
#include <bw/cataplexy/confounder_checker.h>
#include <bw/context.h>
#include <bw/features.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::cataplexy {

// One 1 Hz tick. Pointers are optional sources: nullptr means "no data yet"
// (02 §5 provider convention) — never a magic value.
//
// Freshness is the pump's contract, not this detector's: supply `t_ms` plus the
// StateSnapshot and the MotionFeatures/PpgFeatures produced by the SAME 1 Hz
// tick. D7 gates on a source's presence, wear and quality flags — never on
// age — so pairing a fresh snapshot with stale features would fabricate a
// comparison; keeping the set co-tick is the caller's responsibility.
struct DetectorInput {
    TimeMs t_ms = 0;
    const MotionFeatures* motion = nullptr;
    const PpgFeatures* ppg = nullptr;
    const StateSnapshot* state = nullptr;
};

// What the detector concluded this tick.
struct CandidateOutput {
    TimeMs t_ms = 0;
    bool raised = false;    // confidence >= min -> publish an Event (spec §8)
    bool disabled = false;  // IMU absent/failed -> detector off (spec §10)
    // 0..1. 0 means "nothing was evaluated"; whenever confidence > 0 the
    // reasons mask is non-zero (spec §9: no confidence without reasons).
    float confidence = 0.0f;
    ReasonMask reasons = 0;
    ConfounderMask confounders = 0;
    // Context copied from the snapshot at detection time so the published
    // Event carries the state the candidate was seen in; Unknown when no
    // snapshot was supplied (02 §4.5).
    ActivityContext context = ActivityContext::Unknown;
    AlertnessState alertness_at_start = AlertnessState::Unknown;
    SourceQuality quality{DataQuality::Insufficient, DataQuality::Insufficient};
    TimeMs t_start_ms = 0;  // active window the drop started from
    uint32_t duration_ms = 0;
};

class EventCandidateDetector {
public:
    explicit EventCandidateDetector(const CataplexyConfig& config = CataplexyConfig{})
        : config_(config) {}

    // One step per feature window; returns this tick's conclusion.
    CandidateOutput step(const DetectorInput& input);

    // Drops all retained windows (also used when the device is not worn):
    // the next call reports "no history" instead of comparing across a gap.
    void reset();

    // Windows currently retained (0..kDropHistoryWindows) — tests and debug.
    size_t historyDepth() const { return count_; }

    const CataplexyConfig& config() const { return config_; }

private:
    // Retains the window unless the gap or a backwards timestamp breaks the
    // continuity the drop rule needs (kHistoryMaxGapMs).
    void push(const DetectorInput& input);

    MotionSample makeSample(const DetectorInput& input) const;

    CataplexyConfig config_;
    MotionSample history_[kDropHistoryWindows];  // [0] = newest window
    size_t count_ = 0;
    TimeMs last_t_ms_ = 0;
};

}  // namespace bw::cataplexy
