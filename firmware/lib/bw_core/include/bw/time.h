#pragma once

// bw/time.h — saturating TimeMs arithmetic helpers (F00 spec §6 D6).
//
// Why these exist: raw `now - since` on uint64_t silently underflows to ~1.8e19 ms
// when a caller passes a stale or pre-reset timestamp, which makes every "has enough
// time passed" check pass (03 §4 requires host tests for exactly this; 01 §4 forbids
// 32-bit truncation). All arithmetic here stays uint64_t — no uint32_t duration ever.

#include <bw/types.h>

namespace bw {

// Milliseconds elapsed from `since` to `now`.
// Saturating: returns 0 when now < since (stale/pre-reset timestamp), so a caller
// never sees a bogus ~584-million-year elapsed time.
constexpr TimeMs elapsedMs(TimeMs now, TimeMs since) {
    return (now >= since) ? (now - since) : TimeMs{0};
}

// True when `since` lies at most `window` ms in the past of `now`.
// Inclusive at the boundary: exactly `window` elapsed counts as within.
// Returns false when now < since (stale timestamp), same as elapsedMs.
constexpr bool withinMs(TimeMs now, TimeMs since, TimeMs window) {
    return (now >= since) && ((now - since) <= window);
}

}  // namespace bw
