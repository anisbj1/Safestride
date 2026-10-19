#pragma once

// bw/task_health.h — 02 §7 delegates TaskHealth to F00; definition is F00's spec
// §16, copied verbatim. 01 §3.2 mandates exactly three contents ("last run, max
// lateness, stack high-water mark"); `valid` is 02 §1's validity-flag rule — without
// it, last_run_ms == 0 would be indistinguishable between "ran at boot" and "never
// ran". worst_lateness_ms is uint16_t to match HealthSnapshot::worst_task_lateness_ms
// (02 §5), so no conversion is needed at the boundary.

#include <cstdint>

#include <bw/types.h>

namespace bw {

struct TaskHealth {
    // Default initializers enforce 02 §1's validity rule on ANY construction: a plain
    // `TaskHealth h;` on a task's stack starts in the "no data" state, so an
    // uninitialized variable can never claim valid=true for a cycle that never ran.
    bool valid = false;      // false until the task completes one cycle
    TimeMs last_run_ms = 0;  // monotonic time of the last completed cycle
    uint16_t worst_lateness_ms =
        0;  // worst (actual - expected) period since boot; feeds HealthSnapshot
    // FreeRTOS high-water mark; 0 = not yet measured. Stored as reported (the mark is
    // already a running minimum on the device) — noteCycle never second-guesses it.
    uint16_t stack_free_min_bytes = 0;

    // Records one completed cycle. The max-keeping rule for worst_lateness_ms lives
    // HERE rather than in each feature's wiring: F00 §9 pins "keeps the max, never
    // resets downward", and one shared helper means no task can re-invent it wrong
    // (an on-time cycle must never mask an earlier stall).
    // lateness_ms is uint32_t and SATURATES at UINT16_MAX: a task late by more than
    // the field can hold must record as "very late", never as a truncated small number
    // that would read as on-time.
    void noteCycle(TimeMs now_ms, uint32_t lateness_ms, uint16_t stack_free_bytes) {
        valid = true;
        last_run_ms = now_ms;
        const uint32_t capped = (lateness_ms > UINT16_MAX) ? UINT16_MAX : lateness_ms;
        if (capped > worst_lateness_ms) {
            worst_lateness_ms = static_cast<uint16_t>(capped);
        }
        stack_free_min_bytes = stack_free_bytes;
    }
};

}  // namespace bw
