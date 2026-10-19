#include <bw/reliability/reinit_scheduler.h>

namespace bw::reliability {

bool ReinitScheduler::attemptDue(const TimeMs now_ms, const SensorStatus status) {
    if (status == SensorStatus::Ok) {
        armed_ = false;                         // recovery cancels any pending attempt
        next_attempt_ms_ = 0;                   // ... so nextAttemptMs() honestly says "none"
        backoff_ms_ = kReinitBackoffInitialMs;  // ... and restarts the ladder
        return false;
    }

    if (armed_ && now_ms < next_attempt_ms_) {
        return false;  // still inside the backoff horizon — do not hammer the bus
    }

    // Due: first attempt (nothing armed) or the horizon reached. Schedule the
    // next attempt with x2 growth, capped at kReinitBackoffMaxMs so the ladder
    // is 1 s, 2 s, 4 s, 8 s, 16 s, 30 s, 30 s ... (spec §8/D12).
    armed_ = true;
    next_attempt_ms_ = now_ms + backoff_ms_;
    backoff_ms_ = backoff_ms_ > kReinitBackoffMaxMs / 2 ? kReinitBackoffMaxMs : backoff_ms_ * 2;
    return true;
}

}  // namespace bw::reliability
