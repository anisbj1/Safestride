#pragma once

// bw/state/smoother.h — F07 §7: the EMA behind both evidence channels (D2/D7).
// Seeded on the FIRST valid sample (no warm-up lag from 0.0 — a channel fed
// 0.85 must read 0.85 immediately, not climb to it), reset at gate entry so
// recovery is judged from clean data only (D6/D7). Header-only: 3 fields,
// no platform calls, host-testable by construction.

#include <bw/types.h>

namespace bw::state {

class Smoother {
public:
    explicit Smoother(float alpha) : alpha_(alpha) {}

    // One EMA step; returns the smoothed value.
    float step(float x) {
        if (!seeded_) {
            value_ = x;  // seed: the first sample IS the estimate (D7)
            seeded_ = true;
        } else {
            value_ += alpha_ * (x - value_);
        }
        return value_;
    }

    // Forgets the seed: the next step() re-seeds (gate entry, D6).
    void reset() { seeded_ = false; }

    bool seeded() const { return seeded_; }

    float value() const { return value_; }

private:
    float alpha_;
    float value_ = 0.0f;
    bool seeded_ = false;
};

}  // namespace bw::state
