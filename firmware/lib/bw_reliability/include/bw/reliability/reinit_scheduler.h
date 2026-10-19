#pragma once

// bw/reliability/reinit_scheduler.h — spec §16.1 D12: F26 owns WHEN a sensor
// reinit attempt is due; exponential backoff so a dead bus is never hammered.
//
// Lands UNWIRED (G1 ruling 3): only SensorTask may touch I2C (runtime.md §3.2)
// and F01 exposes no reinit entry point, so no caller exists at M5. This
// module is host-tested here and exposed through f26::reinitScheduler() for a
// later wave's execution seam — nothing here may include F01 headers.

#include <cstdint>

#include <bw/hardware.h>  // SensorStatus
#include <bw/reliability/reliability_config.h>
#include <bw/types.h>

namespace bw::reliability {

// Which sensor's reattempt pacing a scheduler governs (wiring holds one per
// kind; execution is the unwired part).
enum class SensorKind : uint8_t { Imu = 0, Ppg = 1, Count };

class ReinitScheduler {
public:
    // True when a reinit attempt may be made NOW for a sensor reporting
    // `status`:
    //  - Ok         -> never due; any pending attempt is cancelled and the
    //                   backoff resets to kReinitBackoffInitialMs (recovery);
    //  - otherwise  -> the FIRST call is due immediately; every `true` then
    //                   schedules the next attempt backoff_ms later, doubling
    //                   kReinitBackoffInitialMs -> kReinitBackoffMaxMs, i.e.
    //                   1 s, 2 s, 4 s, ... 30 s, 30 s (spec §8/D12).
    // Uninitialized counts as "not Ok": an init attempt is due at once.
    bool attemptDue(TimeMs now_ms, SensorStatus status);

    // Next scheduled attempt (0 = none armed) — diagnostics and tests.
    TimeMs nextAttemptMs() const { return next_attempt_ms_; }

    // Backoff the NEXT `true` result will use — tests assert the doubling.
    uint32_t backoffMs() const { return backoff_ms_; }

private:
    TimeMs next_attempt_ms_ = 0;
    uint32_t backoff_ms_ = kReinitBackoffInitialMs;
    bool armed_ = false;
};

}  // namespace bw::reliability
