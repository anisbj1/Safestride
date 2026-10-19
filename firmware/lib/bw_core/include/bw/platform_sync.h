#pragma once

// bw/platform_sync.h — the ONE preprocessor switch in bw_core (F00 spec §6 D1).
//
// Why: bw_core value types must compile on host and device with no Arduino/FreeRTOS
// dependency (F00 §5), but the mailbox wrapper needs a real critical section on the
// ESP32 (two tasks touch it) while host tests are single-threaded. So the guard is a
// thin class with two definitions: FreeRTOS spinlock under ARDUINO, no-op otherwise.
// Nothing else in bw_core may use #if — containers stay plain, host-testable C++17.
//
// Scope: one global mux guards ALL bw_core guards. Mutual exclusion only holds if
// every participant uses the same lock; per-instance muxes would silently not exclude
// each other. Sections are short (copy one fixed-size struct), so spinning is bounded.
//
// When to use it: when a non-atomic multi-field value must be read consistently while
// another task may write it (e.g. LatestValue). A true single-producer/single-consumer
// ring (SpscQueue) needs only acquire/release atomics — no interrupt masking.

#include <bw/types.h>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace bw {

#if defined(ARDUINO)

// FreeRTOS spinlock critical section (ESP32, Arduino-ESP32 2.x / IDF 4.4).
// portENTER_CRITICAL masks interrupts on the calling core for the section's duration
// — hence the "keep it short" rule above.
inline portMUX_TYPE gBwSyncMux = portMUX_INITIALIZER_UNLOCKED;

class CriticalSection {
public:
    CriticalSection() { portENTER_CRITICAL(&gBwSyncMux); }
    ~CriticalSection() { portEXIT_CRITICAL(&gBwSyncMux); }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
};

#else

// is a no-op. Kept as a real class so code shape is identical on both sides.
class CriticalSection {
public:
    CriticalSection() = default;
    ~CriticalSection() = default;

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
};

#endif

}  // namespace bw
