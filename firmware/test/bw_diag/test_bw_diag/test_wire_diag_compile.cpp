// bw_diag — wiring compile + seam test (plan D5/§15.6, DoD §4.8).
//
// wire_diag.cpp is compiled for the host by including it into this
// translation unit (same-TU include, so its file-static mailbox seam and
// internal helpers are observable). This is the "compiles for native"
// sanity check for the wiring: the ARDUINO-guarded task body and the
// f01:: bindings are excluded, everything else must build and behave.
//
// On the device (P4), the integrator wires F07's pump to the
// f23::latestSourceQuality / f23::latestHealth accessors; before the
// first tick they honestly report "no value yet".

#include "../../../src/app/wiring/wire_diag.cpp"

#include <unity.h>

#include <bw/testkit/recording_fault_reporter.h>

#include "diag_test_support.h"

// Accessors report "no value yet" before the first tick — a tested state,
// exactly like the F07 placeholder (F07 spec:483).
void test_wire_diag_accessors_false_before_first_tick(void) {
    app::AppContext ctx{};
    f23::wire(ctx);  // native: construction only, no task

    bw::SourceQuality q{};
    TEST_ASSERT_FALSE(f23::latestSourceQuality(q));
    bw::HealthSnapshot h{};
    TEST_ASSERT_FALSE(f23::latestHealth(h));
}

// gatherInput on the host (no f01, no stats provider): statuses and
// battery stay invalid — honest unknowns, never fabricated Ok.
void test_wire_diag_gather_input_native_shape(void) {
    app::AppContext ctx{};
    const bw::diag::DiagInput in = f23::gatherInput(ctx, 1000);
    TEST_ASSERT_FALSE(in.has_imu_status);
    TEST_ASSERT_FALSE(in.has_ppg_status);
    TEST_ASSERT_FALSE(in.battery.valid);
    TEST_ASSERT_FALSE(in.stats.valid);
    TEST_ASSERT_EQUAL_UINT64(1000, in.now_ms);
    // M5 (D15): the raw tee exists but is empty on a host that never
    // pushed — absent window, honest "not evaluated" (the device feed is
    // the P4 producer line in wire_hal.cpp).
    TEST_ASSERT_NULL(in.imu_raw);
    TEST_ASSERT_NULL(in.ppg_raw);

    // M5 native shape (§16.4 item 4): no reliability provider attached ⇒
    // has_reliability false (D10/D11 own-only, never a fabricated
    // snapshot); registry mailboxes flow but no slot is noted yet ⇒
    // every slot valid=false (P4 adoption has not run).
    TEST_ASSERT_FALSE(in.has_reliability);
    TEST_ASSERT_FALSE(in.stats.ble_connected);
    TEST_ASSERT_NOT_NULL(in.tasks);
    TEST_ASSERT_EQUAL_UINT8(bw::diag::kMaxTrackedTasks, in.task_count);
    for (uint8_t i = 0; i < in.task_count; ++i) {
        TEST_ASSERT_FALSE(in.tasks[i].valid);
    }

    // ctx mailboxes DO flow through on both platforms.
    ctx.motion_features.set(bw_diag_test::healthyMotion(1000));
    const bw::diag::DiagInput in2 = f23::gatherInput(ctx, 1000);
    TEST_ASSERT_TRUE(in2.has_motion);
}

// The P4 seam: one tick publishes SourceQuality + HealthSnapshot to the
// static mailboxes the accessors read, and forwards report edges to the
// AppContext fault sink (nullptr sink = drop, F01 D7 fallback — checked
// by the third tick below).
void test_wire_diag_tick_publishes_mailboxes_and_reports(void) {
    app::AppContext ctx{};
    bw::testkit::RecordingFaultReporter<16> reporter;
    ctx.faults = &reporter;
    f23::wire(ctx);

    // A stuck IMU raw window is diagnosable on the host: one
    // ImuStuckSensor Error edge goes to the sink. (The raw path needs an
    // Ok driver status — the device always has one via f01::imu().)
    bw::ImuSample stuck[250];
    bw_diag_test::stuckImuWindow(stuck, 250);

    bw::diag::DiagInput in = f23::gatherInput(ctx, 1000);
    in.has_imu_status = true;
    in.imu_status = bw::SensorStatus::Ok;
    in.imu_raw = stuck;
    in.imu_raw_n = 250;
    f23::runTick(ctx, in);

    bw::SourceQuality q{};
    TEST_ASSERT_TRUE(f23::latestSourceQuality(q));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(q.imu));  // stuck ⇒ Insufficient now
    bw::HealthSnapshot h{};
    TEST_ASSERT_TRUE(f23::latestHealth(h));
    TEST_ASSERT_EQUAL_UINT64(1000, h.t_ms);
    TEST_ASSERT_FALSE(h.battery_valid);  // native: no divider, honest unknown
    TEST_ASSERT_FALSE(h.ble_connected);  // F19 is W4 (§15.4)
    TEST_ASSERT_TRUE(h.degraded_mode);

    TEST_ASSERT_EQUAL_UINT8(1, reporter.count());
    TEST_ASSERT_EQUAL_UINT8(0, reporter.dropped());
    TEST_ASSERT_EQUAL_UINT8(1,
                            static_cast<uint8_t>(reporter.countOf(bw::FaultCode::ImuStuckSensor)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(reporter.at(0).severity));

    // Second tick, no raw window: condition cleared, no new report.
    f23::runTick(ctx, f23::gatherInput(ctx, 2000));
    TEST_ASSERT_EQUAL_UINT8(1, reporter.count());
}

// F01 D7 fallback: with ctx.faults == nullptr the edges are dropped, the
// tick never crashes and the mailboxes still update.
void test_wire_diag_null_sink_drops_edges(void) {
    app::AppContext ctx{};  // faults == nullptr
    f23::wire(ctx);

    bw::ImuSample stuck[250];
    bw_diag_test::stuckImuWindow(stuck, 250);
    bw::diag::DiagInput in = f23::gatherInput(ctx, 1000);
    in.has_imu_status = true;
    in.imu_status = bw::SensorStatus::Ok;
    in.imu_raw = stuck;
    in.imu_raw_n = 250;
    f23::runTick(ctx, in);  // must not crash with no sink

    bw::SourceQuality q{};
    TEST_ASSERT_TRUE(f23::latestSourceQuality(q));
}

// ---- M5 seams (spec §16.4 item 4) -------------------------------------------

// D13 round trip: note → mailbox → DiagInput.tasks carries it, with the
// TaskHealth::noteCycle max-keeping rule for lateness and the per-cycle
// watermark as reported. Other slots stay valid=false = excluded.
void test_wire_diag_task_registry_round_trip(void) {
    app::AppContext ctx{};
    f23::noteTaskCycle(bw::diag::TaskSlot::State, 1000, 30, 640);
    f23::noteTaskCycle(bw::diag::TaskSlot::State, 2000, 40, 620);  // later, worse

    bw::TaskHealth reported{};
    TEST_ASSERT_TRUE(f23::latestTaskHealth(bw::diag::TaskSlot::State, reported));
    TEST_ASSERT_TRUE(reported.valid);
    TEST_ASSERT_EQUAL_UINT64(2000, reported.last_run_ms);
    TEST_ASSERT_EQUAL_UINT16(40, reported.worst_lateness_ms);  // max-kept
    TEST_ASSERT_EQUAL_UINT16(620, reported.stack_free_min_bytes);

    const bw::diag::DiagInput in = f23::gatherInput(ctx, 2000);
    TEST_ASSERT_NOT_NULL(in.tasks);
    TEST_ASSERT_EQUAL_UINT8(bw::diag::kMaxTrackedTasks, in.task_count);
    TEST_ASSERT_TRUE(in.tasks[static_cast<uint8_t>(bw::diag::TaskSlot::State)].valid);
    // Excluded: the un-noted Sensor slot contributes nothing to the max.
    TEST_ASSERT_FALSE(in.tasks[static_cast<uint8_t>(bw::diag::TaskSlot::Sensor)].valid);
    TEST_ASSERT_EQUAL_UINT16(40, bw::diag::worstLatenessMs(in.tasks, in.task_count));
    TEST_ASSERT_EQUAL_UINT16(620, bw::diag::worstStackFreeBytes(in.tasks, in.task_count));
    TEST_ASSERT_FALSE(
        bw::diag::slotBelowStackFloor(in.tasks, in.task_count, bw::diag::kMinTaskStackFreeBytes));
}

// D13 accessor honesty: false before a slot's first note ("no data yet"),
// and the Count sentinel is a defined no-op on both sides (02 §1).
void test_wire_diag_latest_task_health_false_before_first_note(void) {
    bw::TaskHealth h{};
    TEST_ASSERT_FALSE(f23::latestTaskHealth(bw::diag::TaskSlot::Comms, h));

    f23::noteTaskCycle(bw::diag::TaskSlot::Count, 1000, 99, 1);  // no write, no crash
    TEST_ASSERT_FALSE(f23::latestTaskHealth(bw::diag::TaskSlot::Count, h));
}

// D15 raw ingest: 64 Imu + 128 Ppg pushes (one full second at F01's
// rates) surface as one drained window each; the following tick is empty
// again (absent window ⇒ §15.4 "not evaluated", never a guess).
void test_wire_diag_raw_ingest_windows_present(void) {
    app::AppContext ctx{};
    bw::ImuSample imu[64];
    bw_diag_test::healthyImuWindow(imu, 64, 1000);
    for (size_t i = 0; i < 64; ++i) {
        f23::onImuSample(imu[i]);
    }
    bw::PpgSample ppg[128];
    bw_diag_test::healthyPpgWindow(ppg, 128, 1000);
    for (size_t i = 0; i < 128; ++i) {
        f23::onPpgSample(ppg[i]);
    }

    const bw::diag::DiagInput in = f23::gatherInput(ctx, 3000);
    TEST_ASSERT_NOT_NULL(in.imu_raw);
    TEST_ASSERT_EQUAL_UINT(64, in.imu_raw_n);
    TEST_ASSERT_NOT_NULL(in.ppg_raw);
    TEST_ASSERT_EQUAL_UINT(128, in.ppg_raw_n);
    // The drain copies real content, not zeros (i == 0 sample spot-check).
    TEST_ASSERT_EQUAL_FLOAT(0.010f, in.imu_raw[0].accel_g[0]);
    TEST_ASSERT_EQUAL_UINT32(40000u, in.ppg_raw[0].red);

    const bw::diag::DiagInput in2 = f23::gatherInput(ctx, 4000);
    TEST_ASSERT_NULL(in2.imu_raw);  // drained: empty ≠ stuck, empty ≠ fault
    TEST_ASSERT_NULL(in2.ppg_raw);
}

// D15 drop-hole rule (revert guard): a REFUSED push (SpscQueue::Full,
// counted by drops()) suppresses THAT source's window on the next tick
// even though samples sit in the queue — the hole ends the raw episode
// instead of being bridged into a fictitious identical-run — while the
// other source's window flows; drops stop changing ⇒ the queue drains
// again (no permanent pinning).
void test_wire_diag_drop_hole_suppresses_window(void) {
    app::AppContext ctx{};
    bw::ImuSample imu[64];
    bw_diag_test::healthyImuWindow(imu, 64, 1000);
    bw::PpgSample ppg[128];
    bw_diag_test::healthyPpgWindow(ppg, 128, 1000);

    for (size_t i = 0; i < 64; ++i) {
        f23::onImuSample(imu[i]);  // fills the IMU tee exactly
    }
    for (size_t i = 0; i < 128; ++i) {
        f23::onPpgSample(ppg[i]);  // fills the PPG tee exactly
    }
    f23::onPpgSample(ppg[0]);  // 129th: REFUSED (Err::Full) → drops() counts it
    // (The refusal is proven below: gatherInput READS drops() — if the
    // counter were ignored, the suppressed window could not appear.)

    const bw::diag::DiagInput in = f23::gatherInput(ctx, 5000);
    TEST_ASSERT_NULL(in.ppg_raw);      // PPG hole ⇒ window suppressed this tick
    TEST_ASSERT_NOT_NULL(in.imu_raw);  // ...while the healthy IMU window flows
    TEST_ASSERT_EQUAL_UINT(64, in.imu_raw_n);

    // The hole tick still DRAINED the 128 queued PPG samples (a hole never
    // clogs the queue — they are "not evaluated", never evaluated with a
    // hole), so this tick has nothing left; recovery is the next PUSH
    // cycle once drops() stops changing.
    const bw::diag::DiagInput in2 = f23::gatherInput(ctx, 6000);
    TEST_ASSERT_NULL(in2.imu_raw);  // IMU already drained by the tick above
    TEST_ASSERT_NULL(in2.ppg_raw);  // hole tick consumed the queue

    for (size_t i = 0; i < 128; ++i) {
        f23::onPpgSample(ppg[i]);  // fresh flow after the hole
    }
    const bw::diag::DiagInput in3 = f23::gatherInput(ctx, 7000);
    TEST_ASSERT_NOT_NULL(in3.ppg_raw);  // not pinned: drops stopped changing
    TEST_ASSERT_EQUAL_UINT(128, in3.ppg_raw_n);

    const bw::diag::DiagInput in4 = f23::gatherInput(ctx, 8000);
    TEST_ASSERT_NULL(in4.ppg_raw);  // empty again: clean state restored

    // Same rule per source: an IMU-side refusal suppresses only IMU.
    for (size_t i = 0; i < 64; ++i) {
        f23::onImuSample(imu[i]);
    }
    f23::onImuSample(imu[0]);  // 65th: refused → IMU hole
    const bw::diag::DiagInput in5 = f23::gatherInput(ctx, 9000);
    TEST_ASSERT_NULL(in5.imu_raw);  // suppressed although 64 samples were queued
    // (the hole tick still drains the queue so a burst never clogs it —
    // the samples are "not evaluated", never evaluated with a hole).
    for (size_t i = 0; i < 10; ++i) {
        f23::onImuSample(imu[i]);  // fresh flow after the hole
    }
    const bw::diag::DiagInput in6 = f23::gatherInput(ctx, 10000);
    TEST_ASSERT_EQUAL_UINT(10, in6.imu_raw_n);  // queue operational again: no pinning
}

// D14 watermark unit (§16.4 verify-at-IMPLEMENT): the IDF 4.4 header says
// uxTaskGetStackHighWaterMark returns BYTES, not vanilla-FreeRTOS words —
// the plan's [ASSUMED] x4 is disproven, the conversion factor is 1, and
// this is its pinned byte-value test.
void test_wire_diag_stack_watermark_unit_is_bytes(void) {
    TEST_ASSERT_EQUAL_UINT32(1, f23::kStackWatermarkUnitBytes);
    TEST_ASSERT_EQUAL_UINT16(512, f23::stackWatermarkBytes(512));  // bytes, not 2048
    TEST_ASSERT_EQUAL_UINT16(1, f23::stackWatermarkBytes(1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, f23::stackWatermarkBytes(0xFFFFFFFFu));  // saturates
}

void test_wire_diag_sensor_status_mailbox_tracks_recovery(void) {
    app::AppContext ctx{};
    f23::noteSensorStatuses(bw::SensorStatus::Ok, bw::SensorStatus::Failed);
    auto in = f23::gatherInput(ctx, 1000);
    TEST_ASSERT_TRUE(in.has_imu_status);
    TEST_ASSERT_TRUE(in.has_ppg_status);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::SensorStatus::Ok), static_cast<int>(in.imu_status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::SensorStatus::Failed),
                          static_cast<int>(in.ppg_status));
    f23::noteSensorStatuses(bw::SensorStatus::Degraded, bw::SensorStatus::Ok);
    in = f23::gatherInput(ctx, 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::SensorStatus::Degraded),
                          static_cast<int>(in.imu_status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::SensorStatus::Ok), static_cast<int>(in.ppg_status));
}

void test_wire_diag_drop_during_drain_suppresses_window(void) {
    // Script the producer interleaving inside pop(): a real queue refuses a
    // concurrent push while full, after the consumer read the initial counter.
    // No threads/sleeps: both raw sources use this exact drain helper.
    struct ScriptedQueue {
        uint32_t lost = 0;
        bool pending = true;
        bool inject_drop = true;
        uint32_t drops() const { return lost; }
        bool pop(bw::ImuSample& out) {
            if (!pending) {
                return false;
            }
            pending = false;
            if (inject_drop) {
                ++lost;
            }
            out.t_ms = 1000;
            return true;
        }
    } queue;
    bw::ImuSample window[1];
    uint32_t seen = 0;
    TEST_ASSERT_EQUAL_UINT(0, f23::drainRawWindow(queue, window, 1, seen));
    TEST_ASSERT_EQUAL_UINT32(1, seen);
    queue.pending = true;
    queue.inject_drop = false;
    TEST_ASSERT_EQUAL_UINT(1, f23::drainRawWindow(queue, window, 1, seen));  // clean recovery
    TEST_ASSERT_EQUAL_UINT64(1000, window[0].t_ms);
}

void test_wire_diag_creation_failure_reports_own_code(void) {
    app::AppContext ctx{};
    bw::testkit::RecordingFaultReporter<4> faults;
    ctx.faults = &faults;
    f23::reportTaskCreateFailure(ctx, 1000);
    TEST_ASSERT_EQUAL_UINT(1, faults.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::TaskCreateFailed),
                          static_cast<int>(faults.at(0).code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(faults.at(0).severity));
    ctx.faults = nullptr;
    f23::reportTaskCreateFailure(ctx, 2000);  // null-safe failure path
}

void test_wire_diag_reliability_provider_value_copy_and_detach(void) {
    struct Provider final : bw::IReliabilityStateProvider {
        bool latest(bw::ReliabilityState& out) const override {
            out.fault_bits = 1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled);
            out.safe_mode = true;
            return true;
        }
    } provider;
    app::AppContext ctx{};
    f23::attachReliabilityProvider(&provider);
    const auto in = f23::gatherInput(ctx, 1000);
    f23::attachReliabilityProvider(nullptr);  // detach before stack provider is destroyed
    TEST_ASSERT_TRUE(in.has_reliability);
    TEST_ASSERT_TRUE(in.reliability.safe_mode);
    TEST_ASSERT_EQUAL_HEX32(1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled),
                            in.reliability.fault_bits);
    TEST_ASSERT_FALSE(f23::gatherInput(ctx, 2000).has_reliability);
}
