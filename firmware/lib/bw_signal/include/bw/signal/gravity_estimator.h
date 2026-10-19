#pragma once

// bw/signal/gravity_estimator.h — F02 §7: gravity via low-pass (F02 D1).
// The gravity vector is the slowly-varying component of acceleration; it is
// kept across sample gaps because it is an orientation prior that re-converges
// in ~2 time constants (F02 D5).

#include <bw/types.h>

namespace bw::signal {

class GravityEstimator {
public:
    // One accelerometer sample in g, sensor frame. Seeded by the first sample,
    // then EMA low-passed with kGravityLpAlpha (one update per nominal sample).
    void update(const float accel_g[3]);

    // Forgets the estimate; the next update() re-seeds it.
    void reset();

    bool initialized() const { return initialized_; }

    // Estimated gravity vector in g (valid when initialized()).
    void vectorG(float out[3]) const;

    // Angle between the estimated gravity vector and the device z-axis, deg.
    // 0 = +z up, 90 = gravity in the x/y plane, 180 = -z. 0 when uninit.
    float tiltDeg() const;

private:
    float g_[3] = {0.0f, 0.0f, 0.0f};
    bool initialized_ = false;
};

}  // namespace bw::signal
