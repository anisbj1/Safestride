// bw/diag/imu_quality_assessor.cpp — F23 IMU diagnosis. See header.

#include <bw/diag/imu_quality_assessor.h>

namespace bw::diag {

bool ImuQualityAssessor::sameFrame(const MotionFeatures& a, const MotionFeatures& b) {
    // t_ms excluded on purpose: frames arrive once per window, so identical
    // CONTENT is the stuck signal, not identical timestamps.
    return a.quality == b.quality && a.enmo_g == b.enmo_g && a.mag_std_g == b.mag_std_g &&
           a.jerk_rms_g_per_s == b.jerk_rms_g_per_s && a.gyro_rms_dps == b.gyro_rms_dps &&
           a.gravity_tilt_deg == b.gravity_tilt_deg && a.dominant_freq_hz == b.dominant_freq_hz &&
           a.stillness_ms == b.stillness_ms;
}

bool ImuQualityAssessor::sameSample(const ImuSample& a, const ImuSample& b) {
    // t_ms excluded: two consecutive healthy samples share the sensor
    // content only when the sensor stopped converting.
    return a.accel_g[0] == b.accel_g[0] && a.accel_g[1] == b.accel_g[1] &&
           a.accel_g[2] == b.accel_g[2] && a.gyro_dps[0] == b.gyro_dps[0] &&
           a.gyro_dps[1] == b.gyro_dps[1] && a.gyro_dps[2] == b.gyro_dps[2];
}

ImuAssessOutput ImuQualityAssessor::step(TimeMs now_ms, bool has_status, SensorStatus status,
                                         const MotionFeatures* features, const ImuSample* raw,
                                         size_t raw_n) {
    ImuAssessOutput out{};

    // Missing / dead sensor: quality only. F01 reports ImuAbsent /
    // ImuReadFailed itself; F23 never double-reports a F01 code (D6).
    if (!has_status || status == SensorStatus::Failed || status == SensorStatus::Uninitialized) {
        reset();  // sensor absence breaks both raw and feature continuity
        out.instantaneous = DataQuality::Insufficient;
        return out;
    }

    // --- stuck check, raw-window path (D6b; M2: host tests + future M5 feed).
    // Runs whenever a window is present, BEFORE the feature-stream check:
    // raw evidence is direct and must not depend on the feature pipeline
    // being alive.
    if (raw != nullptr && raw_n > 0) {
        bool stuck_in_window = false;
        for (size_t i = 0; i < raw_n; ++i) {
            const uint32_t previous_run = raw_run_;
            if (has_prev_raw_ && sameSample(raw[i], prev_raw_)) {
                if (raw_run_ < UINT32_MAX) {
                    ++raw_run_;
                }
            } else {
                raw_run_ = 1;
            }
            prev_raw_ = raw[i];
            has_prev_raw_ = true;
            stuck_in_window = stuck_in_window || (previous_run < config_.stuck_samples &&
                                                  raw_run_ >= config_.stuck_samples);
        }
        if (stuck_in_window || raw_run_ >= config_.stuck_samples) {
            raw_latched_ = true;
        } else {
            raw_latched_ = false;  // run broke — a later episode may re-edge
        }
    } else {
        // No window this tick ⇒ "not evaluated" (D6): the raw episode
        // ends here. A latch must not outlive its evidence — the samples
        // across the gap were never observed, so the run restarts on the
        // next window instead of continuing a count we cannot vouch for
        // (PPG recomputes its latches the same way; M5's capture-gated
        // feed must not pin a source after a capture ends).
        raw_run_ = 0;
        has_prev_raw_ = false;
        raw_latched_ = false;
        raw_edge_emitted_ = false;
    }

    // Feature frames: absent or stale ⇒ the source currently says nothing
    // reliable (fail-safe, §15.4). Not a fault — F26 owns TaskStalled.
    const bool fresh =
        features != nullptr && now_ms >= features->t_ms &&
        (now_ms - features->t_ms) <= static_cast<TimeMs>(config_.feature_staleness_ms);
    if (!fresh) {
        has_prev_frame_ = false;
        frame_run_ = 0;
        frames_latched_ = false;
        frames_edge_emitted_ = false;
        out.instantaneous = DataQuality::Insufficient;
        if (raw_latched_ && !raw_edge_emitted_) {
            out.edge = FaultEdge{FaultCode::ImuStuckSensor, FaultSeverity::Error};
            raw_edge_emitted_ = true;
        }
        return out;
    }

    // --- stuck check, feature-frame path (D6a) ------------------------------
    if (!has_prev_frame_ || features->t_ms != prev_frame_.t_ms) {
        if (has_prev_frame_ && features->t_ms > prev_frame_.t_ms &&
            sameFrame(*features, prev_frame_)) {
            if (frame_run_ < UINT8_MAX) {
                ++frame_run_;
            }
        } else {
            frame_run_ = 1;
        }
        prev_frame_ = *features;
        has_prev_frame_ = true;
    }
    if (frame_run_ >= config_.stuck_feature_windows) {
        frames_latched_ = true;  // condition holds; edge emitted once (below)
    } else {
        frames_latched_ = false;  // run broke — a later episode may re-edge
    }

    // --- edges (one code, strongest evidence first) --------------------------
    if (raw_latched_ && !raw_edge_emitted_) {
        out.edge = FaultEdge{FaultCode::ImuStuckSensor, FaultSeverity::Error};
        raw_edge_emitted_ = true;
    } else if (frames_latched_ && !frames_edge_emitted_) {
        out.edge = FaultEdge{FaultCode::ImuStuckSensor, FaultSeverity::Error};
        frames_edge_emitted_ = true;
    }
    if (raw_latched_ || frames_latched_) {
        out.instantaneous = DataQuality::Insufficient;
        return out;
    }
    // Episode fully cleared: allow the next one to edge again (the emitted
    // flags track the EMISSION, the latched flags the CONDITION).
    if (!raw_latched_) {
        raw_edge_emitted_ = false;
    }
    if (!frames_latched_) {
        frames_edge_emitted_ = false;
    }

    // --- healthy path: worst-of(F01 status hint, F02 quality hint) ----------
    DataQuality own =
        (status == SensorStatus::Degraded) ? DataQuality::Degraded : DataQuality::Good;
    out.instantaneous = features->quality;
    if (static_cast<uint8_t>(own) > static_cast<uint8_t>(out.instantaneous)) {
        out.instantaneous = own;
    }
    return out;
}

void ImuQualityAssessor::reset() {
    has_prev_frame_ = false;
    frame_run_ = 0;
    frames_latched_ = false;
    frames_edge_emitted_ = false;
    has_prev_raw_ = false;
    raw_run_ = 0;
    raw_latched_ = false;
    raw_edge_emitted_ = false;
}

}  // namespace bw::diag
