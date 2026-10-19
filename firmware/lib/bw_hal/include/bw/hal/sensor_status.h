#pragma once

// bw/hal/sensor_status.h — consecutive-failure counter shared by the sensor
// drivers (F01 D8): one pure, host-tested implementation instead of a copy of
// the same counter in Mpu6050Imu, Max30102Ppg, U8g2Canvas, ...
//
// The tracker decides *state transitions only*; the driver owns the vocabulary:
// it maps `readFail() == true` (the transition edge) to its own FaultCode and
// reports it once, and it returns the SensorStatus from status() (02 §3).

#include <cstdint>

#include <bw/hardware.h>

namespace bw::hal {

// [EXPERIMENTAL] Consecutive read failures before SensorStatus::Failed (F01
// spec §8): a single glitch must not flap the status, yet a real disconnect
// must be caught inside ~0.1-0.5 s — at the 50 Hz IMU read rate 5 failures
// take ~100 ms. Shared by every driver so the threshold cannot diverge.
constexpr uint8_t kFailuresBeforeFailed = 5;

class SensorStatusTracker {
public:
    // `failures_before_failed` is kFailuresBeforeFailed (F01 spec §8); 0 is
    // treated as 1 (fail on the first read failure).
    explicit SensorStatusTracker(uint8_t failures_before_failed)
        : failures_before_failed_(failures_before_failed == 0 ? 1 : failures_before_failed) {}

    // begin() / reinit() succeeded -> Ok, streak cleared (reinit after Failed
    // is the F26-driven recovery path; when to call it belongs to F26).
    void beginOk() {
        status_ = bw::SensorStatus::Ok;
        streak_ = 0;
    }

    // begin() returned Err::NotFound (absent at boot, spec §10): declared Failed
    // immediately — no need to wait for N read failures on a device that was
    // never there. The driver reports the *Absent fault code.
    void beginAbsent() {
        status_ = bw::SensorStatus::Failed;
        streak_ = 0;
    }

    // A read succeeded. Also covers "replugged without reinit": the device is
    // answering again, so the state returns to Ok and the streak re-arms.
    void readOk() {
        status_ = bw::SensorStatus::Ok;
        streak_ = 0;
    }

    // A read failed. Returns true exactly on the transition into Failed (the
    // driver reports the fault once per transition — rate limiting by edge, the
    // same rule as I2cBus); false while still Ok or already Failed.
    bool readFail() {
        if (status_ == bw::SensorStatus::Failed) {
            return false;
        }
        if (streak_ < kMaxStreak) {
            ++streak_;
        }
        if (streak_ >= failures_before_failed_) {
            status_ = bw::SensorStatus::Failed;
            return true;
        }
        return false;
    }

    bw::SensorStatus status() const { return status_; }

private:
    // Saturating counter bound: streak is only meaningful below
    // `failures_before_failed_` (uint8_t), this just prevents wrap-around from
    // prolonged failure while status_ handling catches up.
    static constexpr uint8_t kMaxStreak = 255;

    bw::SensorStatus status_ = bw::SensorStatus::Uninitialized;
    uint8_t failures_before_failed_;
    uint8_t streak_ = 0;
};

}  // namespace bw::hal
