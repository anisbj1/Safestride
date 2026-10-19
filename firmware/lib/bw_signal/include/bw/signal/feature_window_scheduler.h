#pragma once

// bw/signal/feature_window_scheduler.h — F02 §7: 1 s cadence + stall/gap
// close (F02 D4). Timestamp-driven window boundaries: a window closes when a
// sample crossed its end (noteSample) or when the stream stalled for longer
// than kMaxSampleGapMs past the boundary (shouldClose with a caller-supplied
// `now` — the processor never reads a clock itself, keeping it host-pure).

#include <bw/signal/signal_config.h>
#include <bw/types.h>

namespace bw::signal {

class FeatureWindowScheduler {
public:
    FeatureWindowScheduler() : window_end_ms_(kMotionWindowMs) {}

    // Called by feed() for every incoming sample of either stream.
    void noteSample(TimeMs t_ms) {
        if (t_ms >= window_end_ms_) {
            boundary_passed_ = true;
        }
    }

    bool boundaryPassed() const { return boundary_passed_; }

    // Close when a sample crossed the boundary, or when `now_ms` is more than
    // kMaxSampleGapMs past it (stalled stream — spec §10: no new samples).
    bool shouldClose(TimeMs now_ms) const {
        return boundary_passed_ || now_ms >= window_end_ms_ + kMaxSampleGapMs;
    }

    TimeMs windowEnd() const { return window_end_ms_; }

    void advance() {
        window_end_ms_ += kMotionWindowMs;
        boundary_passed_ = false;
    }

private:
    TimeMs window_end_ms_;
    bool boundary_passed_ = false;
};

}  // namespace bw::signal
