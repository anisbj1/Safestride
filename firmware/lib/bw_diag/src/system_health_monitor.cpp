// bw/diag/system_health_monitor.cpp — F23 health diagnosis. See header.

#include <bw/diag/system_health_monitor.h>

namespace bw::diag {

// CR-032 mirror: the §8 constant must stay pinned to the enum append.
static_assert(static_cast<uint8_t>(bw::FaultCode::LowStack) == kLowStackFaultCode,
              "kLowStackFaultCode must mirror bw/fault.h FaultCode::LowStack (CR-032)");

namespace {

uint8_t clampPercent(float percent_estimate) {
    if (!(percent_estimate > 0.0f)) {  // also catches NaN: never guess high
        return 0;
    }
    if (percent_estimate >= 100.0f) {
        return 100;
    }
    return static_cast<uint8_t>(percent_estimate);
}

}  // namespace

HealthAssessOutput SystemHealthMonitor::step(const BatteryReading& battery,
                                             const SystemStats& stats, const bw::TaskHealth* tasks,
                                             uint8_t task_count) {
    HealthAssessOutput out{};

    // --- battery (§15.4: valid=false ⇒ unknown, flag not a guess) -----------
    out.battery_valid = battery.valid;
    out.battery_percent = battery.valid ? clampPercent(battery.percent_estimate) : 0;
    uint8_t latched_now = 0;
    if (battery.valid) {
        // Strict "<" per fault.h and D9: exactly 20.0 % is NOT low.
        if (static_cast<float>(out.battery_percent) < config_.critical_battery_percent) {
            latched_now |= kCriticalBatteryBit;
        }
        if (static_cast<float>(out.battery_percent) < config_.low_battery_percent) {
            latched_now |= kLowBatteryBit;
        }
    }  // else: unknown battery clears both conditions (nothing diagnosed)

    // --- heap (§15.4: stats invalid ⇒ unknown, no fault) --------------------
    // Strict "<" for heap (fault.h, D9).
    out.min_free_heap = stats.valid ? stats.min_free_heap : 0;
    if (stats.valid && out.min_free_heap < config_.min_free_heap_bytes) {
        latched_now |= kLowHeapBit;
    }

    // --- task lateness (D13): max(legacy field, valid registry slots) ------
    // The legacy SystemStats field still floors the max (transition period:
    // F07 reports through f07::maxLatenessMs() until its P4 adoption line);
    // INVALID slots contribute nothing — never counted as 0 ms late. Worst
    // stays 0 when there is no evidence at all ⇒ the overrun condition is
    // false: unknown ≠ late.
    const uint16_t legacy_lateness = stats.valid ? stats.worst_task_lateness_ms : 0;
    const uint16_t slots_lateness = worstLatenessMs(tasks, task_count);
    out.worst_task_lateness_ms =
        (slots_lateness > legacy_lateness) ? slots_lateness : legacy_lateness;
    out.ble_connected = stats.valid && stats.ble_connected;  // filled at the seam (D12)
    // Strict ">" for lateness (fault.h, D9).
    if (out.worst_task_lateness_ms > config_.max_task_lateness_ms) {
        latched_now |= kOverrunBit;
    }

    // --- per-task stack watermark (D14, §16.3 LowStack) ---------------------
    // Its own evidence, independent of stats.valid. No valid slot ⇒ false
    // (unknown ≠ 0 free bytes; 0 = not yet measured — task_registry.h).
    if (slotBelowStackFloor(tasks, task_count, config_.min_task_stack_free_bytes)) {
        latched_now |= kLowStackBit;
    }
    latched_ = latched_now;

    // --- edges (one per newly-transitioned condition) ------------------------
    auto push = [&out](FaultCode code, FaultSeverity severity) {
        if (out.edge_count < kMaxFaultEdgesPerTick) {
            out.edges[out.edge_count++] = FaultEdge{code, severity};
        }
    };
    struct Condition {
        uint8_t bit;
        FaultCode code;
        FaultSeverity severity;
    };
    // Fixed order (Critical, Low, Heap, Overrun, Stack): deterministic edge
    // order within a tick keeps host fixtures reproducible.
    static constexpr Condition kConditions[] = {
        {kCriticalBatteryBit, FaultCode::CriticalBattery, FaultSeverity::Error},
        {kLowBatteryBit, FaultCode::LowBattery, FaultSeverity::Warning},
        {kLowHeapBit, FaultCode::LowHeap, FaultSeverity::Warning},
        {kOverrunBit, FaultCode::TaskOverrun, FaultSeverity::Warning},
        {kLowStackBit, FaultCode::LowStack, FaultSeverity::Warning},  // §16.3
    };
    for (const Condition& c : kConditions) {
        if (latched(c.bit)) {
            if (!emitted(c.bit)) {
                push(c.code, c.severity);
                emitted_ |= c.bit;
            }
        } else {
            // Release the emission flag of a cleared condition (next
            // episode re-edges; D9 rate-limiting happens in Diagnostics).
            emitted_ = static_cast<uint8_t>(emitted_ & ~c.bit);
        }
    }
    return out;
}

bool SystemHealthMonitor::degradedMode(DataQuality imu, DataQuality ppg) {
    return imu != DataQuality::Good || ppg != DataQuality::Good;
}

void SystemHealthMonitor::reset() {
    latched_ = 0;
    emitted_ = 0;
}

}  // namespace bw::diag
