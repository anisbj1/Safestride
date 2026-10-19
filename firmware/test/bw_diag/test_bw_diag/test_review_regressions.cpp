#include <bw/diag/imu_quality_assessor.h>
#include <bw/diag/ppg_quality_assessor.h>
#include <unity.h>

#include "diag_test_support.h"

void test_diag_held_feature_frames_are_not_new_stuck_evidence(void) {
    bw::diag::ImuQualityAssessor imu;
    bw::diag::PpgQualityAssessor ppg;
    auto motion = bw_diag_test::healthyMotion(0);
    auto pulse = bw_diag_test::healthyPpg(0, 0.001f);
    for (uint32_t i = 0; i < bw::diag::kStuckFeatureWindows; ++i) {
        const auto m = imu.step(i, true, bw::SensorStatus::Ok, &motion, nullptr, 0);
        const auto p = ppg.step(i, true, bw::SensorStatus::Ok, &pulse, nullptr, 0);
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                              static_cast<int>(m.edge.code));
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(bw::FaultCode::PpgFlatline),
                              static_cast<int>(p.edge.code));
    }
    // Distinct, identical-content windows still earn the original boundary.
    for (uint32_t i = 1; i < bw::diag::kStuckFeatureWindows; ++i) {
        motion.t_ms = pulse.t_ms = i;
        const auto m = imu.step(i, true, bw::SensorStatus::Ok, &motion, nullptr, 0);
        const auto p = ppg.step(i, true, bw::SensorStatus::Ok, &pulse, nullptr, 0);
        if (i + 1 == bw::diag::kStuckFeatureWindows) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                                  static_cast<int>(m.edge.code));
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgFlatline),
                                  static_cast<int>(p.edge.code));
        }
    }
}

void test_diag_ppg_raw_flatline_spans_bounded_windows_and_resets_on_hole(void) {
    bw::diag::PpgQualityAssessor ppg;
    bw::PpgSample window[bw::diag::kDiagRawQueuePpgSamples];
    bw_diag_test::flatlinePpgWindow(window, bw::diag::kDiagRawQueuePpgSamples);
    const auto first = ppg.step(1000, true, bw::SensorStatus::Ok, nullptr, window,
                                bw::diag::kDiagRawQueuePpgSamples);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(first.edge.code));
    const size_t remainder = bw::diag::kPpgFlatlineRawSamples - bw::diag::kDiagRawQueuePpgSamples;
    const auto second = ppg.step(2000, true, bw::SensorStatus::Ok, nullptr, window, remainder);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgFlatline),
                          static_cast<int>(second.edge.code));
    (void)ppg.step(3000, true, bw::SensorStatus::Ok, nullptr, nullptr, 0);
    const auto after_hole = ppg.step(4000, true, bw::SensorStatus::Ok, nullptr, window, remainder);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(after_hole.edge.code));
}

void test_diag_imu_raw_threshold_inside_window_is_not_lost_at_tail(void) {
    bw::diag::ImuQualityAssessor imu;
    bw::ImuSample window[bw::diag::kDiagRawQueueImuSamples];
    bw_diag_test::stuckImuWindow(window, bw::diag::kDiagRawQueueImuSamples);
    uint32_t fed = 0;
    while (fed + bw::diag::kDiagRawQueueImuSamples < bw::diag::kStuckSamples) {
        (void)imu.step(fed, true, bw::SensorStatus::Ok, nullptr, window,
                       bw::diag::kDiagRawQueueImuSamples);
        fed += bw::diag::kDiagRawQueueImuSamples;
    }
    const size_t crossing_count = bw::diag::kStuckSamples - fed;
    window[crossing_count] = bw_diag_test::healthyImuSample(1000, 0.1f);
    const auto out =
        imu.step(1000, true, bw::SensorStatus::Ok, nullptr, window, crossing_count + 1);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::ImuStuckSensor),
                          static_cast<int>(out.edge.code));
}

void test_diag_missing_evidence_breaks_stuck_continuity(void) {
    bw::diag::ImuQualityAssessor imu;
    bw::diag::PpgQualityAssessor ppg;
    auto motion = bw_diag_test::healthyMotion(0);
    auto pulse = bw_diag_test::healthyPpg(0, 0.001f);
    for (uint32_t i = 0; i + 1 < bw::diag::kStuckFeatureWindows; ++i) {
        motion.t_ms = pulse.t_ms = i;
        (void)imu.step(i, true, bw::SensorStatus::Ok, &motion, nullptr, 0);
        (void)ppg.step(i, true, bw::SensorStatus::Ok, &pulse, nullptr, 0);
    }
    (void)imu.step(1000, true, bw::SensorStatus::Ok, nullptr, nullptr, 0);
    (void)ppg.step(1000, true, bw::SensorStatus::Ok, nullptr, nullptr, 0);
    motion.t_ms = pulse.t_ms = 2000;
    const auto m = imu.step(2000, true, bw::SensorStatus::Ok, &motion, nullptr, 0);
    const auto p = ppg.step(2000, true, bw::SensorStatus::Ok, &pulse, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(m.edge.code));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(bw::FaultCode::PpgFlatline),
                          static_cast<int>(p.edge.code));

    bw::ImuSample raw_imu[bw::diag::kDiagRawQueueImuSamples];
    bw::PpgSample raw_ppg[bw::diag::kDiagRawQueuePpgSamples];
    bw_diag_test::stuckImuWindow(raw_imu, bw::diag::kDiagRawQueueImuSamples);
    bw_diag_test::flatlinePpgWindow(raw_ppg, bw::diag::kDiagRawQueuePpgSamples);
    for (uint32_t i = 0; i < 4; ++i) {
        (void)imu.step(i, true, bw::SensorStatus::Ok, nullptr, raw_imu,
                       bw::diag::kDiagRawQueueImuSamples);
        (void)ppg.step(i, true, bw::SensorStatus::Ok, nullptr, raw_ppg,
                       bw::diag::kDiagRawQueuePpgSamples);
    }
    (void)imu.step(3000, true, bw::SensorStatus::Failed, nullptr, nullptr, 0);
    (void)ppg.step(3000, true, bw::SensorStatus::Failed, nullptr, nullptr, 0);
    const auto recovered_imu = imu.step(4000, true, bw::SensorStatus::Ok, nullptr, raw_imu,
                                        bw::diag::kDiagRawQueueImuSamples);
    const auto recovered_ppg = ppg.step(4000, true, bw::SensorStatus::Ok, nullptr, raw_ppg,
                                        bw::diag::kDiagRawQueuePpgSamples);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(recovered_ppg.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(recovered_imu.edge.code));
}
