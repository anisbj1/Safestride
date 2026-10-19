// bw/diag/task_registry.cpp — pure helpers over the bw::TaskHealth
// snapshot array. See task_registry.h for the honesty rules.

#include <bw/diag/task_registry.h>

namespace bw::diag {

namespace {

// Defensive cap: a caller may pass a larger count than the registry table;
// reading past kMaxTrackedTasks would be out-of-bounds, so clamp first.
uint8_t clampedCount(uint8_t count) {
    return (count > kMaxTrackedTasks) ? kMaxTrackedTasks : count;
}

}  // namespace

uint16_t worstLatenessMs(const bw::TaskHealth* tasks, uint8_t count) {
    if (tasks == nullptr) {
        return 0;  // no array = no evidence (first-call/empty state)
    }
    uint16_t worst = 0;
    const uint8_t n = clampedCount(count);
    for (uint8_t i = 0; i < n; ++i) {
        if (tasks[i].valid && tasks[i].worst_lateness_ms > worst) {
            worst = tasks[i].worst_lateness_ms;  // invalid slots excluded
        }
    }
    return worst;
}

uint16_t worstStackFreeBytes(const bw::TaskHealth* tasks, uint8_t count) {
    if (tasks == nullptr) {
        return 0;  // no evidence, never a trigger (see header)
    }
    bool measured = false;
    uint16_t worst = 0;
    const uint8_t n = clampedCount(count);
    for (uint8_t i = 0; i < n; ++i) {
        // valid + measured (> 0): 0 means "not yet measured"
        // (bw/task_health.h) — unknown ≠ 0 free bytes.
        if (!tasks[i].valid || tasks[i].stack_free_min_bytes == 0) {
            continue;
        }
        if (!measured || tasks[i].stack_free_min_bytes < worst) {
            worst = tasks[i].stack_free_min_bytes;
            measured = true;
        }
    }
    return worst;  // 0 until one slot has actually measured
}

bool slotBelowStackFloor(const bw::TaskHealth* tasks, uint8_t count, uint16_t floor_bytes) {
    if (tasks == nullptr) {
        return false;
    }
    const uint8_t n = clampedCount(count);
    for (uint8_t i = 0; i < n; ++i) {
        const bw::TaskHealth& slot = tasks[i];
        if (slot.valid && slot.stack_free_min_bytes > 0 &&
            slot.stack_free_min_bytes < floor_bytes) {  // strict < (fault.h)
            return true;
        }
    }
    return false;
}

}  // namespace bw::diag
