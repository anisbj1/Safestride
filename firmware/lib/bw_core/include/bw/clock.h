#pragma once

// bw/clock.h — docs/02_INTERFACES.md §2: the time source every feature reads.
//
// Two clocks exist (01 §4): monotonic ms since boot (always available) and the
// phone-provided wall clock (0 = unknown before first sync, DEC-009).
// The device implementation of this interface is NOT assigned yet (F00 spec §13,
// open cross-feature question): a naive millis() is 32-bit and wraps at ~49.7 days,
// which would break 01 §4 — F01 or the integrator must assign it before M1.

#include <bw/types.h>

namespace bw {

struct IClock {
    virtual TimeMs nowMs() const = 0;
    virtual UnixSeconds unixSeconds() const = 0;  // 0 if unknown (02 §2)

    // No virtual destructor on purpose (contract shape, 02 §2): instances are owned
    // by the composition root and never deleted through a base pointer, and RTTI is
    // disabled (DEC-002).
};

}  // namespace bw
