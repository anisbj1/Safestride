#pragma once

// bw/cataplexy/confounder_checker.h — F09 §7 ConfounderChecker, spec §6 D2.
//
// Pure function over three already-collected windows: it says *why* a drop
// looks less like a loss of tone (fall, posture change, short burst) and never
// decides whether to raise anything — the detector owns that. F09 is a
// candidate detector: confounders lower confidence, they do not silently
// swallow events (00 §5: probabilities + deterministic rules).
//
// The mask is F09-local because bw_core's ReasonCode has no confounder values:
// CR-009 proposes appending them; until it is accepted the flags travel in
// ConfounderMask and Event::payload[1] (local adapter, contract-change guidance §3.4).

#include <bw/cataplexy/cataplexy_config.h>
#include <bw/features.h>
#include <bw/types.h>

namespace bw::cataplexy {

// Confounder flags as a bitset, same shape as bw::ReasonMask (02 §4.5).
enum class Confounder : uint8_t {
    FallLike = 0,       // uncontrolled descent before the drop (jerk/impact spike)
    PostureChange = 1,  // wrist re-oriented across the drop (sitting down)
    BriefBurst = 2,     // activity lasted less than the full look-back (gesture)
};

using ConfounderMask = uint8_t;

constexpr ConfounderMask confounderBit(Confounder confounder) {
    return static_cast<ConfounderMask>(1u << static_cast<uint8_t>(confounder));
}

// One feature window reduced to the fields the rule needs: fixed-size and
// trivially copyable so the detector keeps a short history of them (02 §1).
struct MotionSample {
    TimeMs t_ms = 0;
    float enmo_g = 0.0f;
    float tilt_deg = 0.0f;
    float jerk_rms_g_per_s = 0.0f;
    // Usable beat only: hr_valid already folds in PpgFeatures::hr_valid and
    // hr_confidence >= kMinHrConfidence (00 §1: no confidence from bad data).
    float hr_bpm = 0.0f;
    bool hr_valid = false;
    bool valid = false;  // false = window unusable (absent or Insufficient)
};

struct ConfounderInput {
    MotionSample current;   // window in which the drop was observed (still)
    MotionSample previous;  // window before it (the active one)
    MotionSample earlier;   // window before that
    bool has_previous = false;
    bool has_earlier = false;
};

// Returns the confounder bitset for one candidate drop; 0 == no objection.
// No allocation, no platform calls: callable from the host test suite.
ConfounderMask checkConfounders(const ConfounderInput& input, const CataplexyConfig& config);

}  // namespace bw::cataplexy
