#pragma once

// bw/eventlog/event_rate_limiter.h — F17 spec §15.2 D7: per-type de-dup
// ("de-duplication, rate limiting" §1). Remembers the monotonic time of the
// last ACCEPTED event per EventType; allow() refuses a repeat of the SAME type
// that arrives less than kEventTypeMinRepeatMs[type] later. This runs at
// admission, BEFORE queueing — priority (§15.3) then governs overflow only, so
// the two mechanisms never interact (D7).
//
// A suppressed repeat returns Err::Ok from the logger (the occurrence is
// represented by the retained event — de-dup, not loss) and is counted in
// EventLogStats::rate_limited.

#include <cstdint>

#include <bw/eventlog/event_priority.h>
#include <bw/events.h>
#include <bw/types.h>

namespace bw::eventlog {

class EventRateLimiter {
public:
    // now_ms = the event's own occurrence time (t_start_ms), or the clock's
    // nowMs() when the producer left it 0. now_ms == 0 means "no time
    // reference" (no clock yet, producer gave no time): the event passes
    // through unrecorded — with no reference there is nothing honest to
    // compare against, and §15.7 requires the limiter never blocks.
    bool allow(bw::EventType type, bw::TimeMs now_ms);

    // Introspection for tests/diagnostics: the last accepted time for `type`,
    // 0 = never accepted with a usable reference.
    bw::TimeMs lastAcceptedMs(bw::EventType type) const;

private:
    // [DERIVED] Why TimeMs-per-EventType (no `seen` flag): allow() only ever
    // records when now_ms != 0, so 0 doubles as "never" — half the state for
    // the same meaning. 12 entries × 8 B = 96 B.
    bw::TimeMs last_accepted_ms_[kEventTypeCount] = {};
};

}  // namespace bw::eventlog
