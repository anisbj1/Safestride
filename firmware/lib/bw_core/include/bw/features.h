#pragma once

// bw/features.h — docs/02_INTERFACES.md §4.2 (processing outputs, F02 → all).
// One struct per 1 s window; rates are [EXPERIMENTAL] starting points (01 §3.3).

#include <bw/quality.h>
#include <bw/types.h>

namespace bw {

// Standard actigraphy motion metric: mean of max(|a| - 1g, 0).
struct MotionFeatures {
    TimeMs t_ms;
    DataQuality quality;
    float enmo_g;     // mean of max(|a|-1g, 0)
    float mag_std_g;  // std of |a|
    float jerk_rms_g_per_s;
    float gyro_rms_dps;
    float gravity_tilt_deg;  // gravity vector to device z-axis (orientation proxy)
    float dominant_freq_hz;  // 0 if none; used by activity classifier
    uint32_t stillness_ms;   // continuous time below motion threshold
};

struct PpgFeatures {
    TimeMs t_ms;
    DataQuality quality;
    bool hr_valid;
    float hr_bpm;
    float hr_confidence;
    float perfusion_index;  // AC/DC of IR
    float ir_dc_level;      // raw DC level (used by wear detection)
    float sqi;              // 0..1 signal-quality index
    bool motion_corrupted;  // IMU says the window was too energetic to trust
    bool hrv_valid;
    float rmssd_ms;  // optional, may stay invalid in v1
};

}  // namespace bw
