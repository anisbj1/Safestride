#pragma once

// bw/state/duration_counter.h — F07 §7: the dwell/recovery counters of D6/D8.
// Semantics that the tests pin (spec §9):
//  - RESETS (never pauses) when inactive: evidence must be CONTINUOUS
//    ("sustained", E1) — one sample below the enter threshold starts over;
//  - saturates at kMaxDurationFieldMs instead of wrapping past 49 days;
//  - runMs()/sustained() project the run up to `now_ms`, so a caller that
//    reads before its next update() still sees a correct, never-underflowed
//    duration (a future or backward now_ms yields the stored run as-is).
// Header-only: no platform calls, host-testable by construction.

#include <bw/state/state_config.h>
#include <bw/types.h>

namespace bw::state {

class DurationCounter {
public:
    // Feeds one sample: `active` = the condition currently holds.
    void update(bool active, TimeMs now_ms) {
        if (!active) {
            running_ = false;  // reset, not pause (D8/E1)
            run_ms_ = 0;
            last_ms_ = now_ms;
            return;
        }
        if (!running_) {
            running_ = true;  // run starts NOW: the first instant counts 0 ms
            run_ms_ = 0;
            last_ms_ = now_ms;
            return;
        }
        if (now_ms < last_ms_) {
            // Time discontinuity (01 §4 says monotonic; a violated contract
            // must not silently extend a dwell): restart the run.
            run_ms_ = 0;
            last_ms_ = now_ms;
            return;
        }
        const TimeMs elapsed = now_ms - last_ms_;
        last_ms_ = now_ms;
        const TimeMs sum = static_cast<TimeMs>(run_ms_) + elapsed;
        run_ms_ = sum > kMaxDurationFieldMs ? kMaxDurationFieldMs : static_cast<uint32_t>(sum);
    }

    // Current run length projected to `now_ms` (saturating; uint32 field).
    uint32_t runMs(TimeMs now_ms) const {
        if (!running_ || now_ms <= last_ms_) {
            return run_ms_;  // inactive, or a future/backward stamp: stored run
        }
        const TimeMs sum = static_cast<TimeMs>(run_ms_) + (now_ms - last_ms_);
        return sum > kMaxDurationFieldMs ? kMaxDurationFieldMs : static_cast<uint32_t>(sum);
    }

    // Inclusive at the boundary: run == needed counts as sustained (spec §9).
    bool sustained(TimeMs now_ms, TimeMs needed_ms) const {
        return running_ && static_cast<TimeMs>(runMs(now_ms)) >= needed_ms;
    }

    void reset() {
        running_ = false;
        run_ms_ = 0;
    }

    bool running() const { return running_; }

private:
    uint32_t run_ms_ = 0;
    TimeMs last_ms_ = 0;
    bool running_ = false;
};

}  // namespace bw::state
