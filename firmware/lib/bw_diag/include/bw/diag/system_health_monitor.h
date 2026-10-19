#pragma once

// bw/diag/system_health_monitor.h — F23 §3/§8: battery, heap, task
// lateness (legacy SystemStats field + the M5 per-task registry, D13),
// BLE state → HealthSnapshot fields plus the
// LowBattery / CriticalBattery / LowHeap / TaskOverrun / LowStack edges
// (§15.3, §16.3).
//
// Honest-degradation rules (§15.4, D9):
//  - battery.valid == false (bench: no divider, A-04) ⇒ battery_percent 0,
//    battery_valid false, NO fault (unknown ≠ fault);
//  - stats.valid == false ⇒ heap field 0, NO heap fault;
//  - lateness evidence is max(legacy stats field, the valid registry
//    slots): the legacy field still floors the max (transition period,
//    D13) and INVALID slots are excluded — never counted as 0 ms late;
//  - LowStack (D14/§16.3): any VALID slot with a measured watermark
//    strictly below kMinTaskStackFreeBytes — no valid slot ⇒ no fault
//    (unknown ≠ 0 free bytes; bw/task_health.h pins 0 = not yet measured);
//  - boundary semantics are strict: LowBattery only when
//    percent < kLowBatteryPercent, CriticalBattery only when
//    percent < kCriticalBatteryPercent, TaskOverrun only when
//    lateness > kMaxTaskLatenessMs (fault.h's own comments);
//  - a condition that clears unlatches, so a later episode edges again
//    (D9 rate-limiting happens in Diagnostics).
//
// Source quality is NOT this module's business: health faults never
// downgrade imu/ppg (§15.3 rows say "no source effect").
// Pure logic (01 §1 rule 5).

#include <bw/diag/diag_config.h>
#include <bw/diag/diag_types.h>
#include <bw/diag/task_registry.h>
#include <bw/hardware.h>
#include <bw/task_health.h>

namespace bw::diag {

struct HealthAssessOutput {
    // HealthSnapshot fields this module fills (t_ms / quality / fault_bits /
    // degraded_mode belong to Diagnostics, §15.1).
    uint8_t battery_percent = 0;
    bool battery_valid = false;
    uint32_t min_free_heap = 0;
    uint16_t worst_task_lateness_ms = 0;
    bool ble_connected = false;
    FaultEdge edges[kMaxFaultEdgesPerTick]{};
    uint8_t edge_count = 0;
};

class SystemHealthMonitor {
public:
    explicit SystemHealthMonitor(const DiagConfig& config = DiagConfig{}) : config_(config) {}

    // One tick; both inputs may carry valid=false for the whole boot
    // (bench builds) — then this module only passes unknown fields through.
    // M5 (§16.1): tasks/task_count add the per-task registry evidence;
    // default nullptr/0 keeps the M2 behavior exactly (D13).
    HealthAssessOutput step(const BatteryReading& battery, const SystemStats& stats,
                            const bw::TaskHealth* tasks = nullptr, uint8_t task_count = 0);

    // D8 working definition: capability reduced by data quality. F26
    // refines this when its degradation table lands.
    static bool degradedMode(DataQuality imu, DataQuality ppg);

    void reset();

private:
    // D8/D9 episode state, one bit per condition (bit-flags keep the ten
    // M2+M5 bools in two bytes — the struct must stay < 64 B per
    // test_diag_sizes_within_budget; plain bools measured exactly 64).
    static constexpr uint8_t kLowBatteryBit = 1u << 0;
    static constexpr uint8_t kCriticalBatteryBit = 1u << 1;
    static constexpr uint8_t kLowHeapBit = 1u << 2;
    static constexpr uint8_t kOverrunBit = 1u << 3;
    static constexpr uint8_t kLowStackBit = 1u << 4;  // D14 (§16.3)

    // Latched = the condition is true this tick; emitted = its edge has
    // been pushed for the current episode (released when it clears).
    bool latched(uint8_t bit) const { return (latched_ & bit) != 0; }
    bool emitted(uint8_t bit) const { return (emitted_ & bit) != 0; }

    DiagConfig config_;
    uint8_t latched_ = 0;
    uint8_t emitted_ = 0;
};

}  // namespace bw::diag
