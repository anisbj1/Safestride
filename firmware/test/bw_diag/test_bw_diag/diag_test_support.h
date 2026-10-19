// bw_diag test support — shared fixtures for the F23 host suite (§15.5).
//
// Fixture families required by spec §9 / plan §15.5: healthy, flat-line,
// saturated, dropout (missing/stale frames), gap-ridden (alternating
// healthy/missing ticks). All values are deterministic; nothing reads a
// clock.

#pragma once

#include <bw/diag/diag_types.h>
#include <bw/diag/diagnostics.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/quality.h>
#include <bw/types.h>

namespace bw_diag_test {

// ---- feature-frame fixtures -------------------------------------------------

inline bw::MotionFeatures healthyMotion(bw::TimeMs t_ms, float jitter = 0.0f) {
    bw::MotionFeatures f{};
    f.t_ms = t_ms;
    f.quality = bw::DataQuality::Good;
    f.enmo_g = 0.030f + jitter;  // a worn limb's typical small motion
    f.mag_std_g = 0.010f;
    f.jerk_rms_g_per_s = 0.020f;
    f.gyro_rms_dps = 5.0f;
    f.gravity_tilt_deg = 12.0f;
    f.dominant_freq_hz = 1.0f;
    f.stillness_ms = 200;
    return f;
}

inline bw::PpgFeatures healthyPpg(bw::TimeMs t_ms, float perfusion_index = 0.004f,
                                  float jitter = 0.0f) {
    bw::PpgFeatures f{};
    f.t_ms = t_ms;
    f.quality = bw::DataQuality::Good;
    f.hr_valid = true;
    f.hr_bpm = 62.0f;
    f.hr_confidence = 0.90f;
    f.perfusion_index = perfusion_index;
    f.ir_dc_level = 50000.0f + jitter;  // ~19 % of the 18-bit MAX30102 full scale
    f.sqi = 0.80f;
    f.motion_corrupted = false;
    f.hrv_valid = false;
    f.rmssd_ms = 0.0f;
    return f;
}

// ---- raw-window fixtures ----------------------------------------------------

// Healthy: every sample differs slightly, never saturated (a real wrist
// pulse modulates IR beat to beat).
inline void healthyPpgWindow(bw::PpgSample* out, size_t n, bw::TimeMs t0 = 1000) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = bw::PpgSample{t0 + i, 40000u + static_cast<uint32_t>(i % 7),
                               50000u + static_cast<uint32_t>(i % 11), false};
    }
}

// Flat-line: bit-identical IR across the whole window (dead sensor).
inline void flatlinePpgWindow(bw::PpgSample* out, size_t n, bw::TimeMs t0 = 1000) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = bw::PpgSample{t0 + i, 40000u, 50000u, false};
    }
}

// Saturated: `saturated_count` samples flagged by the MAX30102 overflow
// bit (fraction = saturated_count / n).
inline void saturatedPpgWindow(bw::PpgSample* out, size_t n, size_t saturated_count,
                               bw::TimeMs t0 = 1000) {
    healthyPpgWindow(out, n, t0);
    for (size_t i = 0; i < saturated_count; ++i) {
        out[i].saturated = true;
    }
}

inline bw::ImuSample healthyImuSample(bw::TimeMs t_ms, float jitter) {
    bw::ImuSample s{};
    s.t_ms = t_ms;
    s.accel_g[0] = 0.01f + jitter;
    s.accel_g[1] = -0.02f + jitter;
    s.accel_g[2] = 0.99f + jitter;
    s.gyro_dps[0] = 1.5f + jitter;
    s.gyro_dps[1] = -0.5f + jitter;
    s.gyro_dps[2] = 0.25f + jitter;
    return s;
}

inline void healthyImuWindow(bw::ImuSample* out, size_t n, bw::TimeMs t0 = 1000) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = healthyImuSample(t0 + i * 20, static_cast<float>(i % 5) * 0.001f);
    }
}

// Stuck: every sample bit-identical (sensor stopped converting).
inline void stuckImuWindow(bw::ImuSample* out, size_t n, bw::TimeMs t0 = 1000) {
    const bw::ImuSample stuck = healthyImuSample(t0, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        out[i] = stuck;
        out[i].t_ms = t0 + i * 20;  // time advances, content does not
    }
}

// ---- whole-tick fixtures ----------------------------------------------------

inline bw::BatteryReading healthyBattery(float percent = 80.0f) {
    bw::BatteryReading b{};
    b.valid = true;
    b.voltage_v = 3.90f;
    b.percent_estimate = percent;
    b.charging_known = true;
    b.charging = false;
    return b;
}

inline bw::BatteryReading unknownBattery() {
    bw::BatteryReading b{};  // bench: no divider, A-04
    return b;
}

inline bw::diag::SystemStats healthyStats() {
    bw::diag::SystemStats s{};
    s.valid = true;
    s.min_free_heap = 90000;
    s.worst_task_lateness_ms = 5;
    s.ble_connected = false;  // false until F19 (§15.4)
    return s;
}

// A fully-healthy DiagInput for one tick: Ok statuses, fresh frames,
// valid battery, valid stats, no raw windows (the M2 device shape, D6).
// The motion frame's content varies with the tick time on purpose: the
// assessor diagnoses bit-identical runs as a stuck sensor (D2), so a
// "healthy" fixture stream must actually move a little.
inline bw::diag::DiagInput healthyInput(bw::TimeMs now_ms) {
    bw::diag::DiagInput in{};
    in.now_ms = now_ms;
    in.has_imu_status = true;
    in.imu_status = bw::SensorStatus::Ok;
    in.has_ppg_status = true;
    in.ppg_status = bw::SensorStatus::Ok;
    in.has_motion = true;
    in.motion = healthyMotion(now_ms, static_cast<float>((now_ms / 1000) % 3) * 0.0005f);
    in.has_ppg = true;
    in.ppg = healthyPpg(now_ms);
    in.battery = healthyBattery();
    in.stats = healthyStats();
    return in;
}

// ---- edge helpers -----------------------------------------------------------

template <class Output> bool hasEdge(const Output& out, bw::FaultCode code) {
    for (uint8_t i = 0; i < out.edge_count; ++i) {
        if (out.edges[i].code == code) {
            return true;
        }
    }
    return false;
}

// ---- task-registry fixtures (§16.4) -----------------------------------------

// A VALID registry slot at the given watermark (noteCycle enforces the
// max-keeping rule for lateness; 0 lateness = on-time cycle).
inline bw::TaskHealth notedTask(uint16_t worst_lateness_ms, uint16_t stack_free_bytes,
                                bw::TimeMs last_run_ms = 1000) {
    bw::TaskHealth h{};
    h.noteCycle(last_run_ms, worst_lateness_ms, stack_free_bytes);
    return h;
}

}  // namespace bw_diag_test
