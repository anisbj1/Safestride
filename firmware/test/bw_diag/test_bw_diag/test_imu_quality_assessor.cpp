// bw_diag — ImuQualityAssessor tests (spec §15.5 item 2; D2/D6 rules and
// the ImuStuckSensor cause/remedy of §15.3 row 1).

#include <bw/diag/diag_config.h>
#include <bw/diag/imu_quality_assessor.h>
#include <unity.h>

#include "diag_test_support.h"

using bw::DataQuality;
using bw::SensorStatus;
using bw::TimeMs;
using bw::diag::ImuQualityAssessor;
using bw_diag_test::healthyImuWindow;
using bw_diag_test::healthyMotion;
using bw_diag_test::stuckImuWindow;

namespace {

constexpr auto kGood = DataQuality::Good;
constexpr auto kDegraded = DataQuality::Degraded;
constexpr auto kInsufficient = DataQuality::Insufficient;

}  // namespace

// Healthy tick: Ok status + fresh, varied frame ⇒ Good, no edge.
void test_imu_healthy_frame_is_good(void) {
    ImuQualityAssessor a;
    const bw::MotionFeatures f = healthyMotion(1000);
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Missing / dead / unstarted sensor ⇒ Insufficient quality ONLY: F01 owns
// ImuAbsent/ImuReadFailed, F23 never double-reports a F01 code (D6).
void test_imu_failed_status_insufficient_no_f23_edge(void) {
    ImuQualityAssessor a;
    const bw::MotionFeatures f = healthyMotion(1000);
    auto out = a.step(1000, false, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));

    out = a.step(2000, true, SensorStatus::Failed, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));

    out = a.step(3000, true, SensorStatus::Uninitialized, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Status Ok but no feature frames published at all ⇒ fail-safe
// Insufficient, no fault (staleness is F26's territory at M5).
void test_imu_no_features_insufficient(void) {
    ImuQualityAssessor a;
    const auto out = a.step(1000, true, SensorStatus::Ok, nullptr, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Dropout: a frame older than kFeatureStalenessMs describes the past.
// Boundary: age == staleness is still fresh, staleness+1 is not.
void test_imu_stale_features_insufficient(void) {
    ImuQualityAssessor a;
    const TimeMs now = 100000;
    const bw::MotionFeatures fresh = healthyMotion(now - bw::diag::kFeatureStalenessMs);
    auto out = a.step(now, true, SensorStatus::Ok, &fresh, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));

    const bw::MotionFeatures stale = healthyMotion(now - bw::diag::kFeatureStalenessMs - 1);
    out = a.step(now, true, SensorStatus::Ok, &stale, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// TimeMs beyond 32 bits: a >49-day-old frame must NOT wrap back to
// "fresh" — the uint64 age stays huge and stale wins.
void test_imu_huge_time_gap_is_stale(void) {
    ImuQualityAssessor a;
    const TimeMs now = 100 + (TimeMs{1} << 32) + 1000;
    const bw::MotionFeatures ancient = healthyMotion(100);
    const auto out = a.step(now, true, SensorStatus::Ok, &ancient, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Cause/remedy §15.3 row ImuStuckSensor: kStuckFeatureWindows (5)
// bit-identical frames ⇒ one Error edge + Insufficient; the next tick is
// silent (edge once per episode), still Insufficient.
void test_imu_identical_frames_x5_stuck_edge_once(void) {
    ImuQualityAssessor a;
    bw::diag::ImuAssessOutput out{};
    for (TimeMs t = 1000; t < 6000; t += 1000) {
        const bw::MotionFeatures f = healthyMotion(t);  // identical content, new t_ms
        out = a.step(t, true, SensorStatus::Ok, &f, nullptr, 0);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(out.edge.severity));

    const bw::MotionFeatures f6 = healthyMotion(6000);
    out = a.step(6000, true, SensorStatus::Ok, &f6, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(out.edge.code));  // silent while it holds
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
}

// x4 identical frames is below the run length: no fault, still Good.
void test_imu_identical_frames_x4_no_fault(void) {
    ImuQualityAssessor a;
    bw::diag::ImuAssessOutput out{};
    for (TimeMs t = 1000; t < 5000; t += 1000) {
        const bw::MotionFeatures f = healthyMotion(t);
        out = a.step(t, true, SensorStatus::Ok, &f, nullptr, 0);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
}

// Raw path: one window of kStuckSamples (250) identical samples ⇒ fault;
// 249 identical ⇒ below threshold, none. (Raw feed is host-only at M2,
// D6; device feed lands at M5.)
void test_imu_raw_run_250_faults_249_does_not(void) {
    {
        ImuQualityAssessor a;
        bw::ImuSample window[250];
        stuckImuWindow(window, 250);
        const bw::MotionFeatures f = healthyMotion(1000);
        const auto out = a.step(1000, true, SensorStatus::Ok, &f, window, 250);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                              static_cast<int>(out.edge.code));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    }
    {
        ImuQualityAssessor a;
        bw::ImuSample window[249];
        stuckImuWindow(window, 249);
        const bw::MotionFeatures f = healthyMotion(1000);
        const auto out = a.step(1000, true, SensorStatus::Ok, &f, window, 249);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                              static_cast<int>(out.edge.code));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
    }
}

// Recovery: a clean window releases the latch, a later stuck episode
// edges again (the Diagnostics layer applies the D9 rate limit on top).
void test_imu_raw_recovery_allows_reedge(void) {
    ImuQualityAssessor a;
    bw::ImuSample stuck[250];
    bw::ImuSample healthy[50];
    stuckImuWindow(stuck, 250);
    healthyImuWindow(healthy, 50);
    const bw::MotionFeatures f = healthyMotion(1000);

    const auto out1 = a.step(1000, true, SensorStatus::Ok, &f, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out1.edge.code));

    const auto out2 = a.step(2000, true, SensorStatus::Ok, &f, healthy, 50);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out2.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out2.instantaneous));

    const auto out3 = a.step(3000, true, SensorStatus::Ok, &f, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out3.edge.code));  // new episode
}

// Audit L3 / §15.4: an absent raw window is "not evaluated" and ENDS the
// open raw episode — a latch may not outlive its evidence, so quality
// follows the feature stream again and the next window must earn its own
// run from scratch (249 identical samples no longer ride an old count).
void test_imu_absent_raw_window_ends_stuck_episode(void) {
    ImuQualityAssessor a;
    bw::ImuSample stuck[250];
    stuckImuWindow(stuck, 250);
    const bw::MotionFeatures f = healthyMotion(1000);

    const auto out1 = a.step(1000, true, SensorStatus::Ok, &f, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out1.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out1.instantaneous));

    // No window this tick: the episode ends, quality is the feature
    // stream's answer again (frame run = 2 < kStuckFeatureWindows).
    const auto out2 = a.step(2000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out2.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out2.instantaneous));

    // The run restarted at the gap: 249 identical samples is below
    // kStuckSamples again (no stale count carried across the gap).
    bw::ImuSample short_run[249];
    stuckImuWindow(short_run, 249);
    const auto out3 = a.step(3000, true, SensorStatus::Ok, &f, short_run, 249);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out3.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out3.instantaneous));
}

// After an absent-window gap the next full stuck run is a NEW episode and
// edges again (the D9 rate limit in Diagnostics bounds the flapping).
void test_imu_raw_episode_reedges_after_absent_gap(void) {
    ImuQualityAssessor a;
    bw::ImuSample stuck[250];
    stuckImuWindow(stuck, 250);
    const bw::MotionFeatures f = healthyMotion(1000);

    const auto out1 = a.step(1000, true, SensorStatus::Ok, &f, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out1.edge.code));

    const auto out2 = a.step(2000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out2.edge.code));

    const auto out3 = a.step(3000, true, SensorStatus::Ok, &f, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out3.edge.code));  // new episode
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out3.instantaneous));
}

// Audit L4 (§15.4 raw-window rules): stale features + a raw window — the
// two evidence streams combine exactly as documented. A stuck raw run
// edges ImuStuckSensor on the STALE path (raw evidence beats feature
// staleness) while quality stays Insufficient; a healthy raw window does
// not vouch for the missing feature stream.
void test_imu_stale_features_plus_raw_window(void) {
    ImuQualityAssessor a;
    bw::ImuSample stuck[250];
    bw::ImuSample healthy[50];
    stuckImuWindow(stuck, 250);
    healthyImuWindow(healthy, 50);
    const bw::MotionFeatures stale = healthyMotion(1000 - bw::diag::kFeatureStalenessMs - 1);

    // Stuck window + stale features: edge on the stale path, Insufficient.
    auto out = a.step(1000, true, SensorStatus::Ok, &stale, stuck, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));

    // Healthy window + stale features: no fault, still Insufficient —
    // raw samples cannot vouch for the 1 Hz feature stream.
    out = a.step(2000, true, SensorStatus::Ok, &stale, healthy, 50);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
}

// First-call/empty state: everything absent ⇒ Insufficient, no edge,
// no crash (host-test §2 "first call" case).
void test_imu_first_call_empty_state(void) {
    ImuQualityAssessor a;
    const auto out = a.step(0, false, SensorStatus::Uninitialized, nullptr, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Worst-of: F02's own quality hint can only lower, never raise.
void test_imu_f02_hint_degraded_worst_of(void) {
    ImuQualityAssessor a;
    bw::MotionFeatures f = healthyMotion(1000);
    f.quality = kDegraded;
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
}

// Worst-of: a Degraded driver status alone degrades the source.
void test_imu_degraded_status_worst_of(void) {
    ImuQualityAssessor a;
    const bw::MotionFeatures f = healthyMotion(1000);
    const auto out = a.step(1000, true, SensorStatus::Degraded, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
}
