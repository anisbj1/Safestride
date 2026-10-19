// bw_diag — Diagnostics composer tests (spec §15.5 item 5): §10
// fail-safe, composition + edge cap, D9 rate limit, D8 sticky fault_bits
// and degraded_mode, and an end-to-end staleness/hysteresis timeline.

#include <cstdio>

#include <bw/diag/diag_config.h>
#include <bw/diag/diagnostics.h>
#include <unity.h>

#include "diag_test_support.h"

using bw::DataQuality;
using bw::FaultCode;
using bw::TimeMs;
using bw::diag::DiagInput;
using bw::diag::Diagnostics;
using bw::diag::DiagOutput;
using bw::diag::FaultEdge;
using bw_diag_test::flatlinePpgWindow;
using bw_diag_test::hasEdge;
using bw_diag_test::healthyBattery;
using bw_diag_test::healthyInput;
using bw_diag_test::healthyMotion;
using bw_diag_test::healthyPpg;
using bw_diag_test::healthyPpgWindow;
using bw_diag_test::healthyStats;
using bw_diag_test::stuckImuWindow;
using bw_diag_test::unknownBattery;

// §10 fail-safe: empty input ⇒ Insufficient/Insufficient, battery unknown
// (flag, not guess), degraded_mode true, t_ms carried through.
void test_diag_fail_safe_empty_input(void) {
    Diagnostics d;
    DiagInput in{};  // everything absent
    in.now_ms = 1234;
    const DiagOutput out = d.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.ppg));
    TEST_ASSERT_FALSE(out.health.battery_valid);
    TEST_ASSERT_TRUE(out.health.degraded_mode);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_EQUAL_UINT64(1234, out.health.t_ms);
}

// Healthy timeline: the boot state (Insufficient) needs
// kGoodRecoverWindows healthy ticks before the first Good publish.
void test_diag_healthy_timeline_reaches_good(void) {
    Diagnostics d;
    DiagOutput out{};
    out = d.step(healthyInput(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.imu));
    out = d.step(healthyInput(1000));
    out = d.step(healthyInput(2000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Good), static_cast<int>(out.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Good), static_cast<int>(out.quality.ppg));
    TEST_ASSERT_FALSE(out.health.degraded_mode);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_EQUAL_UINT32(0, out.health.fault_bits);
    TEST_ASSERT_EQUAL_UINT64(2000, out.health.t_ms);
}

// Composition: one tick can carry every condition at once — 1 IMU + 1 PPG
// + 4 health edges — and stays within kMaxFaultEdgesPerTick.
void test_diag_composition_and_edge_cap(void) {
    Diagnostics d;
    DiagInput in = healthyInput(1000);
    in.battery = healthyBattery(5.0f);     // critical + low
    in.stats.min_free_heap = 1000;         // low heap
    in.stats.worst_task_lateness_ms = 60;  // overrun
    bw::ImuSample stuck[250];
    stuckImuWindow(stuck, 250);
    in.imu_raw = stuck;
    in.imu_raw_n = 250;
    bw::PpgSample saturated[100];
    bw_diag_test::saturatedPpgWindow(saturated, 100, 2);
    in.ppg_raw = saturated;
    in.ppg_raw_n = 100;

    const DiagOutput out = d.step(in);
    TEST_ASSERT_EQUAL_UINT8(6, out.edge_count);
    TEST_ASSERT_TRUE(out.edge_count <= bw::diag::kMaxFaultEdgesPerTick);
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::ImuStuckSensor));
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::PpgSaturated));
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::CriticalBattery));
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::LowBattery));
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::LowHeap));
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::TaskOverrun));
    // Faulty sources gate quality immediately (D7 Insufficient entry).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.ppg));
}

// D9 rate limit: a flapping LowBattery re-edges are suppressed until
// kFaultRepeatMs after the LAST REPORT; after that the re-edge passes.
void test_diag_rate_limit_suppresses_flapping_reedge(void) {
    Diagnostics d;
    uint8_t low_battery_edges = 0;

    // t=1000: first report goes out.
    DiagInput in = healthyInput(1000);
    in.battery = healthyBattery(19.0f);
    low_battery_edges += hasEdge(d.step(in), FaultCode::LowBattery) ? 1 : 0;

    // t=2000: battery unknown — condition clears.
    in = healthyInput(2000);
    in.battery = unknownBattery();
    (void)d.step(in);

    // t=3000: flapping back — re-edge SUPPRESSED (3000 < 1000 + 60000).
    in = healthyInput(3000);
    in.battery = healthyBattery(19.0f);
    low_battery_edges += hasEdge(d.step(in), FaultCode::LowBattery) ? 1 : 0;

    // t=4000: clears again; t=60999: re-edge still suppressed
    // (60999 < 61000).
    in = healthyInput(4000);
    in.battery = unknownBattery();
    (void)d.step(in);
    in = healthyInput(60999);
    in.battery = healthyBattery(19.0f);
    low_battery_edges += hasEdge(d.step(in), FaultCode::LowBattery) ? 1 : 0;

    // t=61000: clears; t=61500: 61500 >= 61000 — the re-edge passes.
    in = healthyInput(61000);
    in.battery = unknownBattery();
    (void)d.step(in);
    in = healthyInput(61500);
    in.battery = healthyBattery(19.0f);
    low_battery_edges += hasEdge(d.step(in), FaultCode::LowBattery) ? 1 : 0;

    TEST_ASSERT_EQUAL_UINT8(2, low_battery_edges);
}

// D8 sticky fault_bits: once diagnosed, the bit stays for the boot even
// after the condition clears (and even when a later re-edge is
// rate-limit-suppressed).
void test_diag_sticky_fault_bits(void) {
    Diagnostics d;
    DiagInput in = healthyInput(1000);
    in.battery = healthyBattery(19.0f);
    DiagOutput out = d.step(in);
    const uint32_t low_battery_bit = 1u << static_cast<uint8_t>(FaultCode::LowBattery);
    TEST_ASSERT_TRUE((out.health.fault_bits & low_battery_bit) != 0);

    in = healthyInput(2000);  // recovered: 80 %
    out = d.step(in);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_TRUE((out.health.fault_bits & low_battery_bit) != 0);  // sticky
    TEST_ASSERT_TRUE(out.health.fault_bits == d.faultBits());
}

// I2 / D8 hardening: fault_bits is a uint32_t mask, so a code >=
// kFaultBitsWidth must be a DEFINED no-op — no shift >= 32 (UB), no bit,
// no report edge — while in-range codes keep flowing through the same
// path (including foreign codes 1..10, 19..25 and 27..31, which record
// bits but are not rate-limited; LowStack 26 became F23-owned with
// CR-032 and uses slot kF23FaultCodeCount).
void test_diag_out_of_range_fault_code_is_noop(void) {
    Diagnostics d;
    DiagOutput out{};

    d.consider(
        FaultEdge{static_cast<FaultCode>(bw::diag::kFaultBitsWidth), bw::FaultSeverity::Warning},
        1000, out);
    d.consider(FaultEdge{static_cast<FaultCode>(200), bw::FaultSeverity::Error}, 2000, out);
    TEST_ASSERT_EQUAL_UINT32(0, d.faultBits());
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);

    // A foreign but representable code still records its bit.
    d.consider(FaultEdge{static_cast<FaultCode>(3), bw::FaultSeverity::Warning}, 3000, out);
    TEST_ASSERT_TRUE((d.faultBits() & (1u << 3)) != 0);

    // And an F23 code still goes through the full D9 path.
    d.consider(FaultEdge{FaultCode::ImuStuckSensor, bw::FaultSeverity::Error}, 4000, out);
    TEST_ASSERT_TRUE((d.faultBits() & (1u << static_cast<uint8_t>(FaultCode::ImuStuckSensor))) !=
                     0);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);  // foreign code 3 never appended an edge
    // (Foreign = 1..10, 19..25 and 27..31; LowStack(26) is F23-owned since
    // CR-032, so it — like 11..18 — DOES get an edge and the D9 rate limit.)
}

// degraded_mode follows the committed quality immediately when a source
// dies (D8 working definition; D7 Insufficient enters at once).
void test_diag_degraded_mode_on_imu_loss(void) {
    Diagnostics d;
    DiagInput in = healthyInput(1000);
    (void)d.step(in);
    (void)d.step(healthyInput(2000));
    DiagOutput out = d.step(healthyInput(3000));
    TEST_ASSERT_FALSE(out.health.degraded_mode);

    in = healthyInput(4000);
    in.has_imu_status = false;  // IMU dropped out
    out = d.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.imu));
    TEST_ASSERT_TRUE(out.health.degraded_mode);
}

// End-to-end timeline: Good after recovery, immediate Insufficient on a
// dropout (frame stops updating), immediate recovery entry then Good
// again after kGoodRecoverWindows healthy ticks.
void test_diag_end_to_end_staleness_hysteresis_timeline(void) {
    Diagnostics d;
    DiagOutput out{};
    for (TimeMs t = 0; t <= 2000; t += 1000) {  // boot: 3 healthy ticks
        out = d.step(healthyInput(t));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Good), static_cast<int>(out.quality.imu));

    // Dropout: the PPG frame stops publishing; its age crosses
    // kFeatureStalenessMs mid-loop (age == staleness stays fresh, +1 is
    // stale — the boundary).
    for (TimeMs t = 3000; t <= 8000; t += 1000) {
        DiagInput in = healthyInput(t);
        in.ppg = healthyPpg(0);  // frozen at t=0; age grows with t
        out = d.step(in);
        const bool fresh = (t - 0) <= static_cast<TimeMs>(bw::diag::kFeatureStalenessMs);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(fresh ? DataQuality::Good : DataQuality::Insufficient),
            static_cast<int>(out.quality.ppg));
    }
    TEST_ASSERT_TRUE(out.health.degraded_mode);

    // Recovery: fresh frames again — 3 healthy ticks restore Good.
    out = d.step(healthyInput(9000));
    out = d.step(healthyInput(10000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.ppg));
    out = d.step(healthyInput(11000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Good), static_cast<int>(out.quality.ppg));
    TEST_ASSERT_FALSE(out.health.degraded_mode);
}

// Budget (§11 / §15.7): the whole diagnostics state (monitors + rate-limit
// table + config) must stay far below 1 KB. Also prints the measured
// sizes into the assertion message for the report.
void test_diag_sizes_within_budget(void) {
    char msg[120];
    const unsigned diag = static_cast<unsigned>(sizeof(Diagnostics));
    const unsigned cfg = static_cast<unsigned>(sizeof(bw::diag::DiagConfig));
    const unsigned imu = static_cast<unsigned>(sizeof(bw::diag::ImuQualityAssessor));
    const unsigned ppg = static_cast<unsigned>(sizeof(bw::diag::PpgQualityAssessor));
    const unsigned hlth = static_cast<unsigned>(sizeof(bw::diag::SystemHealthMonitor));
    const unsigned hyst = static_cast<unsigned>(sizeof(bw::diag::QualityHysteresis));
    const unsigned in_sz = static_cast<unsigned>(sizeof(bw::diag::DiagInput));

    // The composer owns every module + the rate-limit table. §11 (M5)
    // budgets the WHOLE static footprint (lib + wiring statics) at ≤12 KB;
    // composer + config together keep their own 1 KB bound here. Measured
    // values are carried in the assertion messages for the status log.
    (void)snprintf(msg, sizeof(msg), "Diagnostics+DiagConfig = %u + %u B", diag, cfg);
    TEST_ASSERT_TRUE_MESSAGE(diag + cfg < 1024, msg);
    (void)snprintf(msg, sizeof(msg), "ImuQualityAssessor = %u B", imu);
    TEST_ASSERT_TRUE_MESSAGE(imu < 256, msg);
    (void)snprintf(msg, sizeof(msg), "PpgQualityAssessor = %u B", ppg);
    TEST_ASSERT_TRUE_MESSAGE(ppg < 256, msg);
    (void)snprintf(msg, sizeof(msg), "SystemHealthMonitor = %u B", hlth);
    TEST_ASSERT_TRUE_MESSAGE(hlth < 64, msg);
    (void)snprintf(msg, sizeof(msg), "QualityHysteresis = %u B", hyst);
    TEST_ASSERT_TRUE_MESSAGE(hyst < 128, msg);
    // M5 (§16.4): DiagInput grew +has_reliability/+ReliabilityState
    // (24 B),/+tasks (+8 B),/+task_count — plan estimate ≈216 B; the M2
    // 512 B bound tightens to 256 B (an asserted bound change with §16
    // justification, never a loosening).
    (void)snprintf(msg, sizeof(msg), "DiagInput (M5) = %u B, plan ≈216", in_sz);
    TEST_ASSERT_TRUE_MESSAGE(in_sz < 256, msg);
}

// ---- M5 D11 merge (CR-030) — spec §16.4 item 1 -------------------------------

// Own-only fallback pinned: no provider ⇒ fault_bits is exactly F23's own
// sticky bits; a foreign registry bit (19 = TaskStalled, F26's) is never
// fabricated. With the provider present the snapshot is the union, the
// foreign bit never produces an edge or enters F23's rate limiter (I2
// through the new path), and a provider that vanishes drops the foreign
// bits while the own ones stay sticky — honest "what the registry says now".
void test_diag_fault_bits_own_union_and_vanishing_provider(void) {
    Diagnostics d;
    const uint32_t own_bit = 1u << static_cast<uint8_t>(FaultCode::LowBattery);
    const uint32_t foreign_bit = 1u << static_cast<uint8_t>(FaultCode::TaskStalled);

    // 1) has_reliability=false: own-only, LowBattery diagnosed this tick.
    DiagInput in = healthyInput(1000);
    in.battery = healthyBattery(19.0f);
    DiagOutput out = d.step(in);
    TEST_ASSERT_FALSE(in.has_reliability);
    TEST_ASSERT_EQUAL_UINT32(own_bit, out.health.fault_bits);
    TEST_ASSERT_EQUAL_UINT8(1, out.edge_count);

    // 2) provider present: union — foreign bit visible in the snapshot,
    //    but NO foreign edge and no re-edge of the held LowBattery.
    in = healthyInput(2000);
    in.battery = healthyBattery(19.0f);
    in.has_reliability = true;
    in.reliability = bw::ReliabilityState{2000, foreign_bit, 0, false};
    out = d.step(in);
    TEST_ASSERT_EQUAL_UINT32(own_bit | foreign_bit, out.health.fault_bits);
    TEST_ASSERT_EQUAL_UINT8(0, out.edge_count);
    TEST_ASSERT_FALSE(hasEdge(out, FaultCode::TaskStalled));

    // 3) sticky union across ticks: registry still reports foreign ⇒ still there.
    in = healthyInput(3000);
    in.battery = healthyBattery(19.0f);
    in.has_reliability = true;
    in.reliability = bw::ReliabilityState{3000, foreign_bit, 0, false};
    out = d.step(in);
    TEST_ASSERT_EQUAL_UINT32(own_bit | foreign_bit, out.health.fault_bits);

    // 4) provider vanishes: own sticky survives, foreign is gone — and it
    //    never entered F23's own state (faultBits() was and stays own-only).
    in = healthyInput(4000);
    in.battery = healthyBattery(19.0f);  // has_reliability defaults false
    out = d.step(in);
    TEST_ASSERT_EQUAL_UINT32(own_bit, out.health.fault_bits);
    TEST_ASSERT_EQUAL_UINT32(own_bit, d.faultBits());
}

// degraded_mode matrix (§16.4 item 1): quality-only with no provider (M2
// fallback preserved), provider clean ⇒ quality-only, facility mask ⇒
// true, safe_mode ⇒ true, and no provider + quality down ⇒ true.
void test_diag_degraded_mode_matrix(void) {
    Diagnostics d;
    DiagOutput out{};
    for (TimeMs t = 0; t <= 2000; t += 1000) {  // boot → Good/Good
        out = d.step(healthyInput(t));
    }
    TEST_ASSERT_FALSE(out.health.degraded_mode);  // no provider, quality Good

    // Provider present and clean: the D8 quality definition stands alone.
    DiagInput in = healthyInput(3000);
    in.has_reliability = true;
    in.reliability = bw::ReliabilityState{3000, 0, 0, false};
    out = d.step(in);
    TEST_ASSERT_FALSE(out.health.degraded_mode);

    // CR-029 facility mask ⇒ degraded.
    in = healthyInput(4000);
    in.has_reliability = true;
    in.reliability = bw::ReliabilityState{
        4000, 0,
        static_cast<bw::DegradedMode>(1u << static_cast<uint8_t>(bw::DegradedFacility::PpgLost)),
        false};
    out = d.step(in);
    TEST_ASSERT_TRUE(out.health.degraded_mode);

    // safe_mode ⇒ degraded (even with an empty facility mask).
    in = healthyInput(5000);
    in.has_reliability = true;
    in.reliability = bw::ReliabilityState{5000, 0, 0, true};
    out = d.step(in);
    TEST_ASSERT_TRUE(out.health.degraded_mode);

    // No provider + quality down: the M2 fallback still holds.
    in = healthyInput(6000);
    in.has_imu_status = false;  // IMU dropped → Insufficient immediately
    out = d.step(in);
    TEST_ASSERT_TRUE(out.health.degraded_mode);
}

// ---- M5 D15 cross-tick raw accumulation — spec §16.4 item 5 ------------------

// Five consecutive 50-sample windows of one identical run reach
// kStuckSamples (250) ⇒ exactly one ImuStuckSensor edge (on the 5th tick)
// and the source goes Insufficient.
void test_diag_cross_tick_raw_windows_reach_stuck(void) {
    Diagnostics d;
    bw::ImuSample window[50];
    uint8_t edge_count = 0;
    DiagOutput out{};
    for (uint8_t tick = 1; tick <= 5; ++tick) {
        const TimeMs t = static_cast<TimeMs>(tick) * 1000;
        DiagInput in = healthyInput(t);
        stuckImuWindow(window, 50, t);  // content identical across ticks
        in.imu_raw = window;
        in.imu_raw_n = 50;
        out = d.step(in);
        edge_count += hasEdge(out, FaultCode::ImuStuckSensor) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT8(1, edge_count);  // 250 reached on tick 5 only
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality.imu));
    TEST_ASSERT_TRUE(
        (out.health.fault_bits & (1u << static_cast<uint8_t>(FaultCode::ImuStuckSensor))) != 0);
}

// The drop-hole rule's composer half: an absent window between raw
// windows (what gatherInput passes when the tee dropped) ENDS the raw
// episode — 4+4 identical windows with a hole between never reach
// kStuckSamples, while the consecutive run of test 5 does. The wiring
// half (drops ⇒ suppression) is pinned in test_wire_diag_compile.cpp.
void test_diag_absent_window_between_raw_windows_restarts_run(void) {
    Diagnostics d;
    bw::ImuSample window[50];
    uint8_t edge_count = 0;

    for (uint8_t tick = 1; tick <= 4; ++tick) {  // 4 x 50 = 200 < 250
        DiagInput in = healthyInput(static_cast<TimeMs>(tick) * 1000);
        stuckImuWindow(window, 50, static_cast<TimeMs>(tick) * 1000);
        in.imu_raw = window;
        in.imu_raw_n = 50;
        edge_count += hasEdge(d.step(in), FaultCode::ImuStuckSensor) ? 1 : 0;
    }

    DiagInput hole = healthyInput(5000);  // window suppressed: nullptr/0
    edge_count += hasEdge(d.step(hole), FaultCode::ImuStuckSensor) ? 1 : 0;

    for (uint8_t tick = 6; tick <= 9; ++tick) {  // run restarted: 200 again
        DiagInput in = healthyInput(static_cast<TimeMs>(tick) * 1000);
        stuckImuWindow(window, 50, static_cast<TimeMs>(tick) * 1000);
        in.imu_raw = window;
        in.imu_raw_n = 50;
        edge_count += hasEdge(d.step(in), FaultCode::ImuStuckSensor) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT8(0, edge_count);  // hole never bridged into a 250-run
}

// ---- M5 D14 LowStack through the composer — spec §16.3/§16.4 -----------------

// LowStack flows the full path (monitor → consider → edges + sticky bit)
// and honors D9: the flapping re-edge inside kFaultRepeatMs is suppressed
// (the bit stays visible), the re-edge after the window passes.
void test_diag_low_stack_edge_reported_and_rate_limited(void) {
    Diagnostics d;
    const uint32_t low_stack_bit = 1u << static_cast<uint8_t>(FaultCode::LowStack);
    const bw::TaskHealth low[] = {bw_diag_test::notedTask(5, 511)};
    const bw::TaskHealth ok[] = {bw_diag_test::notedTask(5, 700)};

    // t=1000: first episode ⇒ edge + sticky bit.
    DiagInput in = healthyInput(1000);
    in.tasks = low;
    in.task_count = 1;
    DiagOutput out = d.step(in);
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::LowStack));
    TEST_ASSERT_TRUE((out.health.fault_bits & low_stack_bit) != 0);

    // t=2000: recovery (monitor clears), t=3000: re-drop inside
    // kFaultRepeatMs ⇒ suppressed (bit still set).
    in = healthyInput(2000);
    in.tasks = ok;
    in.task_count = 1;
    (void)d.step(in);
    in = healthyInput(3000);
    in.tasks = low;
    in.task_count = 1;
    out = d.step(in);
    TEST_ASSERT_FALSE(hasEdge(out, FaultCode::LowStack));
    TEST_ASSERT_TRUE((out.health.fault_bits & low_stack_bit) != 0);

    // t=61000: recovery; t=61500: re-drop ≥ kFaultRepeatMs after the
    // t=1000 report ⇒ the re-edge passes.
    in = healthyInput(61000);
    in.tasks = ok;
    in.task_count = 1;
    (void)d.step(in);
    in = healthyInput(61500);
    in.tasks = low;
    in.task_count = 1;
    out = d.step(in);
    TEST_ASSERT_TRUE(hasEdge(out, FaultCode::LowStack));
}
