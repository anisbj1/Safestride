#include <bw/eventlog/event_rate_limiter.h>

namespace bw::eventlog {

bool EventRateLimiter::allow(bw::EventType type, bw::TimeMs now_ms) {
    const size_t index = static_cast<size_t>(type);
    if (index >= kEventTypeCount || now_ms == 0) {
        // Future EventType (append-only) or no time reference: pass through,
        // record nothing (header contract).
        return true;
    }
    const bw::TimeMs interval = kEventTypeMinRepeatMs[index];
    if (interval == 0) {
        last_accepted_ms_[index] = now_ms;  // keep the newest time for diagnostics
        return true;
    }
    const bw::TimeMs last = last_accepted_ms_[index];
    if (last != 0 && now_ms >= last && (now_ms - last) < interval) {
        return false;  // same type inside its §15.3 window → suppressed
    }
    // last == 0 (first) or a backwards monotonic jump (cannot happen: TimeMs is
    // monotonic within a boot, 02 §1) → accept.
    last_accepted_ms_[index] = now_ms;
    return true;
}

bw::TimeMs EventRateLimiter::lastAcceptedMs(bw::EventType type) const {
    const size_t index = static_cast<size_t>(type);
    return index < kEventTypeCount ? last_accepted_ms_[index] : 0;
}

}  // namespace bw::eventlog
