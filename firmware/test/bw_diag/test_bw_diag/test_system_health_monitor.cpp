// bw_diag — SystemHealthMonitor tests (spec §15.5 item 4; §15.3 rows
// LowBattery / CriticalBattery / LowHeap / TaskOverrun incl. the strict
// boundary semantics pinned by fault.h, D8 degraded_mode, and the honest
// unknown-not-fault rule of §15.4).

#include <bw/diag/diag_config.h>
#include <bw/diag/system_health_monitor.h>
#include <unity.h>

#include "diag_test_support.h"

using bw::BatteryReading;
using bw::diag::HealthAssessOutput;
using bw::diag::SystemHealthMonitor;
using bw::diag::SystemStats;
using bw_diag_test::hasEdge;
using bw_diag_test::healthyBattery;
using bw_diag_test::healthyStats;
using bw_diag_test::unknownBattery;

// Bench build (A-04, no divider): battery.valid=false is a FLAG, not a
// guess — percent 0, valid false, no battery fault.
void test_health_invalid_battery_is_flag_only(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(unknownBattery(), healthyStats());
    TEST_ASSERT_FALSE(out.battery_valid);
    TEST_ASSERT_EQUAL_UINT8(0, out.battery_percent);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowBattery));
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::CriticalBattery));
}

// 25 % is comfortably above both thresholds: nothing fires.
void test_health_battery_25_none(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(25.0f), healthyStats());
    TEST_ASSERT_TRUE(out.battery_valid);
    TEST_ASSERT_EQUAL_UINT8(25, out.battery_percent);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowBattery));
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::CriticalBattery));
}

// 19.9 % < 20 % ⇒ LowBattery Warning (§15.3 remedy: charge; F12 shows it).
void test_health_low_battery_19_9_warns(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(19.9f), healthyStats());
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::LowBattery));
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::CriticalBattery));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Warning),
                          static_cast<int>(out.edges[0].severity));
}

// Boundary pinned by fault.h (D9): exactly 20.0 % is NOT low.
void test_health_low_battery_boundary_20_0_silent(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(20.0f), healthyStats());
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowBattery));
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::CriticalBattery));
}

// 7.9 % fires BOTH: CriticalBattery Error + LowBattery Warning (each
// threshold is strict "<", and the two diagnoses are independent).
void test_health_critical_7_9_fires_both(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(7.9f), healthyStats());
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::CriticalBattery));
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::LowBattery));
    TEST_ASSERT_EQUAL_UINT8(2, out.edge_count);
}

// Boundary: exactly 8.0 % is NOT critical, still low.
void test_health_critical_boundary_8_0_low_only(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(8.0f), healthyStats());
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::CriticalBattery));
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::LowBattery));
}

// Heap floor: 16383 B < kMinFreeHeapBytes ⇒ LowHeap Warning; 16384 B ⇒
// none (strict "<").
void test_health_low_heap_16383_vs_16384(void) {
    SystemHealthMonitor m;
    SystemStats stats = healthyStats();

    stats.min_free_heap = 16383;
    HealthAssessOutput out = m.step(healthyBattery(), stats);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::LowHeap));
    TEST_ASSERT_EQUAL_UINT32(16383, out.min_free_heap);

    SystemHealthMonitor fresh;
    stats.min_free_heap = 16384;
    out = fresh.step(healthyBattery(), stats);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowHeap));
}

// Lateness floor: 51 ms > kMaxTaskLatenessMs ⇒ TaskOverrun Warning;
// 50 ms ⇒ none (strict ">", §15.3 row TaskOverrun remedy: profile the
// late task; F26 owns the system response).
void test_health_overrun_51_vs_50(void) {
    SystemHealthMonitor m;
    SystemStats stats = healthyStats();

    stats.worst_task_lateness_ms = 51;
    HealthAssessOutput out = m.step(healthyBattery(), stats);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::TaskOverrun));

    SystemHealthMonitor fresh;
    stats.worst_task_lateness_ms = 50;
    out = fresh.step(healthyBattery(), stats);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::TaskOverrun));
}

// stats.valid=false (§15.4): fields reported as unknown (0), NO fault.
void test_health_invalid_stats_no_faults(void) {
    SystemHealthMonitor m;
    SystemStats stats{};        // valid=false
    stats.min_free_heap = 100;  // garbage-in must not become a diagnosis
    stats.worst_task_lateness_ms = 200;
    const HealthAssessOutput out = m.step(healthyBattery(), stats);
    TEST_ASSERT_EQUAL_UINT32(0, out.min_free_heap);
    TEST_ASSERT_EQUAL_UINT16(0, out.worst_task_lateness_ms);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowHeap));
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::TaskOverrun));
}

// Persistent condition, edge once: a held-low battery does not re-report
// every tick (D9 edge semantics; Diagnostics' rate limit sits on top).
void test_health_no_rereport_while_condition_holds(void) {
    SystemHealthMonitor m;
    const HealthAssessOutput out1 = m.step(healthyBattery(10.0f), healthyStats());
    TEST_ASSERT_TRUE(hasEdge(out1, bw::FaultCode::LowBattery));

    const HealthAssessOutput out2 = m.step(healthyBattery(10.0f), healthyStats());
    TEST_ASSERT_EQUAL_UINT8(0, out2.edge_count);
}

// Recovery unlatches: charging to 50 % then dropping again re-edges.
void test_health_recharge_then_drop_reedges(void) {
    SystemHealthMonitor m;
    TEST_ASSERT_TRUE(
        hasEdge(m.step(healthyBattery(10.0f), healthyStats()), bw::FaultCode::LowBattery));
    TEST_ASSERT_EQUAL_UINT8(0, m.step(healthyBattery(50.0f), healthyStats()).edge_count);
    TEST_ASSERT_TRUE(
        hasEdge(m.step(healthyBattery(10.0f), healthyStats()), bw::FaultCode::LowBattery));
}

// D8 working definition of degraded_mode: capability reduced by data
// quality on EITHER source.
void test_health_degraded_mode_definition(void) {
    TEST_ASSERT_FALSE(
        SystemHealthMonitor::degradedMode(bw::DataQuality::Good, bw::DataQuality::Good));
    TEST_ASSERT_TRUE(
        SystemHealthMonitor::degradedMode(bw::DataQuality::Degraded, bw::DataQuality::Good));
    TEST_ASSERT_TRUE(
        SystemHealthMonitor::degradedMode(bw::DataQuality::Good, bw::DataQuality::Insufficient));
}

// ---- M5 per-task registry (spec §16.4 item 2; D13/D14) ----------------------

// Worst lateness = max(legacy stats field, valid slots): the registry's
// 60 ms wins over F07's legacy 5 ms and crosses the overrun threshold —
// per-task evidence must raise the diagnosis the legacy field missed.
void test_health_worst_lateness_max_over_valid_slots(void) {
    SystemHealthMonitor m;
    const bw::TaskHealth slots[] = {bw_diag_test::notedTask(60, 700),
                                    bw_diag_test::notedTask(20, 800), bw::TaskHealth{}};
    const HealthAssessOutput out = m.step(healthyBattery(), healthyStats(), slots, 3);
    TEST_ASSERT_EQUAL_UINT16(60, out.worst_task_lateness_ms);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::TaskOverrun));  // 60 > 50
}

// INVALID slots are excluded, never counted as 0 (or as their garbage
// contents): with only an invalid slot holding 300 ms and an on-time
// valid slot, the max is the legacy field's 5 ms and nothing fires.
void test_health_invalid_slots_excluded_from_lateness(void) {
    SystemHealthMonitor m;
    SystemStats stats = healthyStats();
    stats.worst_task_lateness_ms = 5;

    bw::TaskHealth slots[2]{};
    slots[0].worst_lateness_ms = 300;  // valid=false garbage: excluded
    slots[1] = bw_diag_test::notedTask(0, 900);

    const HealthAssessOutput out = m.step(healthyBattery(), stats, slots, 2);
    TEST_ASSERT_EQUAL_UINT16(5, out.worst_task_lateness_ms);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::TaskOverrun));
}

// Transition period (D13): the legacy SystemStats field still floors the
// max — F07's 55 ms outranks a valid slot's 10 ms and still fires.
void test_health_legacy_stats_field_floors_max(void) {
    SystemHealthMonitor m;
    SystemStats stats = healthyStats();
    stats.worst_task_lateness_ms = 55;
    const bw::TaskHealth slots[] = {bw_diag_test::notedTask(10, 900)};
    const HealthAssessOutput out = m.step(healthyBattery(), stats, slots, 1);
    TEST_ASSERT_EQUAL_UINT16(55, out.worst_task_lateness_ms);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::TaskOverrun));
}

// The §16.4 51/50 boundary re-pinned through the tasks-array path (the
// legacy field is absent — stats invalid — but registry evidence alone is
// enough: 51 > kMaxTaskLatenessMs fires, 50 does not).
void test_health_overrun_via_tasks_array_51_50(void) {
    SystemStats no_stats{};  // invalid: heap/legacy unknown
    const bw::TaskHealth late[] = {bw_diag_test::notedTask(51, 900)};
    const bw::TaskHealth ontime[] = {bw_diag_test::notedTask(50, 900)};

    SystemHealthMonitor m;
    const HealthAssessOutput out = m.step(healthyBattery(), no_stats, late, 1);
    TEST_ASSERT_EQUAL_UINT16(51, out.worst_task_lateness_ms);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::TaskOverrun));

    SystemHealthMonitor fresh;
    const HealthAssessOutput out2 = fresh.step(healthyBattery(), no_stats, ontime, 1);
    TEST_ASSERT_EQUAL_UINT16(50, out2.worst_task_lateness_ms);
    TEST_ASSERT_FALSE(hasEdge(out2, bw::FaultCode::TaskOverrun));
}

// §16.3 LowStack boundary: 511 < kMinTaskStackFreeBytes ⇒ Warning edge
// (and nothing else fires); 512 ⇒ silent (strict "<", fault.h).
void test_health_low_stack_511_vs_512(void) {
    SystemHealthMonitor m;
    const bw::TaskHealth low[] = {bw_diag_test::notedTask(5, 511)};
    const HealthAssessOutput out = m.step(healthyBattery(), healthyStats(), low, 1);
    TEST_ASSERT_TRUE(hasEdge(out, bw::FaultCode::LowStack));
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);  // only LowStack
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Warning),
                          static_cast<int>(out.edges[0].severity));

    SystemHealthMonitor fresh;
    const bw::TaskHealth ok[] = {bw_diag_test::notedTask(5, 512)};
    TEST_ASSERT_FALSE(
        hasEdge(fresh.step(healthyBattery(), healthyStats(), ok, 1), bw::FaultCode::LowStack));
}

// Two tasks below the floor ⇒ ONE LowStack edge per episode (not one per
// task), none while it holds, and a full recovery re-arms the edge.
void test_health_low_stack_multi_task_single_edge_per_episode(void) {
    SystemHealthMonitor m;
    const bw::TaskHealth low[] = {bw_diag_test::notedTask(5, 100), bw_diag_test::notedTask(5, 200)};

    const HealthAssessOutput out1 = m.step(healthyBattery(), healthyStats(), low, 2);
    TEST_ASSERT_TRUE(hasEdge(out1, bw::FaultCode::LowStack));
    TEST_ASSERT_EQUAL_UINT8(1, out1.edge_count);

    const HealthAssessOutput out2 = m.step(healthyBattery(), healthyStats(), low, 2);
    TEST_ASSERT_EQUAL_UINT8(0, out2.edge_count);  // persistent: no re-report

    const bw::TaskHealth recovered[] = {bw_diag_test::notedTask(5, 700)};
    const HealthAssessOutput out3 = m.step(healthyBattery(), healthyStats(), recovered, 1);
    TEST_ASSERT_EQUAL_UINT8(0, out3.edge_count);  // cleared, no edge

    const HealthAssessOutput out4 = m.step(healthyBattery(), healthyStats(), low, 2);
    TEST_ASSERT_TRUE(hasEdge(out4, bw::FaultCode::LowStack));  // new episode re-edges
}

// A VALID slot that never measured its watermark (0 = "not yet measured",
// bw/task_health.h) is unknown — unknown ≠ 0 free bytes: no LowStack.
void test_health_low_stack_unmeasured_zero_silent(void) {
    SystemHealthMonitor m;
    const bw::TaskHealth unmeasured[] = {bw_diag_test::notedTask(5, 0)};
    const HealthAssessOutput out = m.step(healthyBattery(), healthyStats(), unmeasured, 1);
    TEST_ASSERT_FALSE(hasEdge(out, bw::FaultCode::LowStack));
}
