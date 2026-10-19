#pragma once

// bw/wear/wear_detector.h — F04 §1/§6: WearState machine over WearEvidence.
//
// Bias (D2): starts Uncertain, never Worn without positive evidence (§5).
// Hysteresis + dwell (D1): a state change needs the score past an enter/exit
// threshold AND that candidate to hold for its confirm dwell; evidence in
// between keeps the current state. "No usable source at all" drops to
// Uncertain immediately (§10) — there is no evidence to wait for.
//
// Pure logic: Output step(const Input&), time comes in as TimeMs, no
// FreeRTOS/Arduino calls (01 §1 rule 5).

#include <bw/context.h>
#include <bw/features.h>
#include <bw/types.h>
#include <bw/wear/wear_config.h>
#include <bw/wear/wear_evidence.h>

namespace bw::wear {

struct WearInput {
    TimeMs t_ms;                             // tick time (one step per 1 Hz feature window)
    const PpgFeatures* ppg = nullptr;        // nullptr = source absent
    const MotionFeatures* motion = nullptr;  // nullptr = source absent
};

class WearDetector {
public:
    explicit WearDetector(const WearConfig& config = WearConfig{}) : config_(config) {}

    // One 1 Hz tick; returns the committed estimate for this tick.
    WearEstimate step(const WearInput& input);

    // Current committed state (for tests/debugging).
    WearState state() const { return state_; }

private:
    // Weighted blend of the available sources (D1/D4); one source alone
    // when the other is absent.
    float combinedScore(const WearEvidence& evidence) const;

    // Threshold state: >= enter -> Worn, <= exit -> NotWorn, inside the
    // band -> hold the current state (hysteresis, so Uncertain stays
    // Uncertain until evidence clears a threshold).
    WearState candidateState(float score) const;

    // Dwell a candidate must hold before it commits; Worn and NotWorn use
    // their confirm times, Uncertain is unreachable as a pending candidate.
    TimeMs dwellFor(WearState candidate) const;

    void resetCandidate();

    WearConfig config_;
    WearState state_ = WearState::Uncertain;  // D2: default bias
    bool candidate_pending_ = false;
    TimeMs candidate_since_ms_ = 0;
};

}  // namespace bw::wear
