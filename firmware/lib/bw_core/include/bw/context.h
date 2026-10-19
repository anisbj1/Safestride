#pragma once

// bw/context.h — docs/02_INTERFACES.md §4.3 (context estimates, F03/F04).
//
// The classifier must not equate low movement with sleep (00 §F03 note): posture
// values may be reported with low confidence or collapsed to Stationary when the
// wrist alone cannot decide. Wear/activity are independent axes (DEC-004).

#include <cstdint>

#include <bw/types.h>

namespace bw {

enum class ActivityContext : uint8_t {
    Unknown,
    Stationary,
    Sitting,
    Standing,
    Walking,
    Running,
    LyingResting,
    InactiveProlonged,
};

struct ActivityEstimate {
    TimeMs t_ms;
    ActivityContext context;
    float confidence;  // 0..1, model/heuristic confidence
};

enum class WearState : uint8_t { Uncertain, Worn, NotWorn };

struct WearEstimate {
    TimeMs t_ms;
    WearState state;
    float confidence;  // 0..1
};

}  // namespace bw
