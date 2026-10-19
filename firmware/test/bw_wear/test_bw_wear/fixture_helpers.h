#pragma once

// Shared scenario fixtures for the bw_wear / F04 suite (private helper,
// owned by F04's test folder — host-testing guidance: private helpers stay in
// test/<lib>/). Every fixture stamps t_ms so the freshness rule in D3 is
// exercised on purpose.
//
// Fixture -> score map (see status log for the fixture table):
//   ppgAttached  0.985   ppgMoving 0.74   ppgLoose 0.461
//   ppgAmbient   0.0075  ppgNoData absent
//   motionRest   0.496   motionDesk 0.30  motionDeskLongStill 0.0
//   motionMoving 1.00    motionInsufficient absent

#include <bw/context.h>
#include <bw/features.h>
#include <bw/quality.h>
#include <bw/types.h>

namespace f04test {

// --- PPG -------------------------------------------------------------------

// Worn, attached, resting: solid DC, firm pulse, valid HR, good SQI.
inline bw::PpgFeatures ppgAttached(bw::TimeMs t_ms) {
    bw::PpgFeatures p{};
    p.t_ms = t_ms;
    p.quality = bw::DataQuality::Good;
    p.hr_valid = true;
    p.hr_bpm = 70.0f;
    p.hr_confidence = 0.9f;
    p.perfusion_index = 0.01f;
    p.ir_dc_level = 120000.0f;
    p.sqi = 0.9f;
    return p;
}

// Worn but moving: contact holds, motion kills the beat/SQI.
inline bw::PpgFeatures ppgMoving(bw::TimeMs t_ms) {
    bw::PpgFeatures p{};
    p.t_ms = t_ms;
    p.quality = bw::DataQuality::Degraded;
    p.perfusion_index = 0.008f;
    p.ir_dc_level = 110000.0f;
    p.sqi = 0.6f;
    p.motion_corrupted = true;
    return p;
}

// Worn with a lifted/loose sensor: DC present, pulse barely above noise
// (F02 would call this Insufficient — the D3 case we must not discard).
inline bw::PpgFeatures ppgLoose(bw::TimeMs t_ms) {
    bw::PpgFeatures p{};
    p.t_ms = t_ms;
    p.quality = bw::DataQuality::Insufficient;
    p.perfusion_index = 0.0015f;
    p.ir_dc_level = 90000.0f;
    p.sqi = 0.2f;
    return p;
}

// On a desk: ambient light leak only, no pulse, no perfusion.
inline bw::PpgFeatures ppgAmbient(bw::TimeMs t_ms) {
    bw::PpgFeatures p{};
    p.t_ms = t_ms;
    p.quality = bw::DataQuality::Insufficient;
    p.ir_dc_level = 6000.0f;
    p.sqi = 0.05f;
    return p;
}

// Sensor stalled / no samples: F02 zero-fills DC when coverage fails.
inline bw::PpgFeatures ppgNoData(bw::TimeMs t_ms) {
    bw::PpgFeatures p{};
    p.t_ms = t_ms;
    p.quality = bw::DataQuality::Insufficient;
    return p;
}

// --- IMU -------------------------------------------------------------------

// Worn, resting: micro-motion above the desk floor, stillness timer young.
inline bw::MotionFeatures motionRest(bw::TimeMs t_ms) {
    bw::MotionFeatures m{};
    m.t_ms = t_ms;
    m.quality = bw::DataQuality::Good;
    m.enmo_g = 0.012f;
    m.mag_std_g = 0.011f;
    m.gravity_tilt_deg = 45.0f;
    m.stillness_ms = 3000;
    return m;
}

// On a surface, placed recently: near-zero variance, young stillness timer.
inline bw::MotionFeatures motionDesk(bw::TimeMs t_ms) {
    bw::MotionFeatures m{};
    m.t_ms = t_ms;
    m.quality = bw::DataQuality::Good;
    m.enmo_g = 0.001f;
    m.mag_std_g = 0.001f;
    m.gravity_tilt_deg = 90.0f;
    m.stillness_ms = 60000;
    return m;
}

// On a surface, untouched for minutes: stillness past kDeskStillnessMs.
inline bw::MotionFeatures motionDeskLongStill(bw::TimeMs t_ms) {
    bw::MotionFeatures m{};
    m.t_ms = t_ms;
    m.quality = bw::DataQuality::Good;
    m.enmo_g = 0.001f;
    m.mag_std_g = 0.001f;
    m.gravity_tilt_deg = 90.0f;
    m.stillness_ms = 400000;
    return m;
}

// Worn/held and moving: clearly above the desk floor, no stillness.
inline bw::MotionFeatures motionMoving(bw::TimeMs t_ms) {
    bw::MotionFeatures m{};
    m.t_ms = t_ms;
    m.quality = bw::DataQuality::Good;
    m.enmo_g = 0.15f;
    m.mag_std_g = 0.20f;
    m.jerk_rms_g_per_s = 0.05f;
    m.gyro_rms_dps = 40.0f;
    m.gravity_tilt_deg = 30.0f;
    m.dominant_freq_hz = 1.8f;
    m.stillness_ms = 0;
    return m;
}

// IMU window below coverage: statistics undefined, nothing to score.
inline bw::MotionFeatures motionInsufficient(bw::TimeMs t_ms) {
    bw::MotionFeatures m{};
    m.t_ms = t_ms;
    m.quality = bw::DataQuality::Insufficient;
    return m;
}

}  // namespace f04test
