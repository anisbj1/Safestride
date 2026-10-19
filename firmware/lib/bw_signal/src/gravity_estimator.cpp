#include <bw/signal/gravity_estimator.h>

#include <cmath>

#include <bw/signal/signal_config.h>

namespace bw::signal {

void GravityEstimator::update(const float accel_g[3]) {
    if (!initialized_) {
        g_[0] = accel_g[0];
        g_[1] = accel_g[1];
        g_[2] = accel_g[2];
        initialized_ = true;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        g_[i] += kGravityLpAlpha * (accel_g[i] - g_[i]);
    }
}

void GravityEstimator::reset() {
    g_[0] = 0.0f;
    g_[1] = 0.0f;
    g_[2] = 0.0f;
    initialized_ = false;
}

void GravityEstimator::vectorG(float out[3]) const {
    out[0] = g_[0];
    out[1] = g_[1];
    out[2] = g_[2];
}

float GravityEstimator::tiltDeg() const {
    if (!initialized_) {
        return 0.0f;
    }
    const float mag = std::sqrt(g_[0] * g_[0] + g_[1] * g_[1] + g_[2] * g_[2]);
    if (mag <= 0.0f) {
        return 0.0f;  // degenerate zero vector — no orientation to report
    }
    float cos_tilt = g_[2] / mag;
    if (cos_tilt > 1.0f) {
        cos_tilt = 1.0f;
    }
    if (cos_tilt < -1.0f) {
        cos_tilt = -1.0f;
    }
    return std::acos(cos_tilt) * kRadiansToDegrees;
}

}  // namespace bw::signal
