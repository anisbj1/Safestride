// test/bw_wake/test_bw_wake/test_heuristic_estimator.cpp — F06 §9: table
// tests for the heuristic rules, monotonicity, validity and reason codes.

#include <cmath>
#include <limits>

#include <unity.h>

#include "wake_test_support.h"

using namespace bw;
using namespace bw::wake;
using namespace bw::wake::test;

namespace {

// Builds a history whose newest slot is `epoch` (the assembler invariant).
EpochHistory historyFor(const EpochSummary& epoch) {
    EpochHistory history{};
    pushHistory(history, epoch);
    return history;
}

bool hasReason(ReasonMask mask, ReasonCode code) {
    return (mask & (static_cast<ReasonMask>(1) << static_cast<uint8_t>(code))) != 0;
}

}  // namespace

void test_heuristic_walking_reads_as_awake(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.enmo_low_ratio = 0.0f;  // every window had motion
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(out.probs.awake > out.probs.drowsy);
    TEST_ASSERT_TRUE(out.probs.awake > out.probs.sleep);
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::SustainedLowMotion));
}

void test_heuristic_sustained_stillness_reads_as_drowsy(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.enmo_low_ratio = 1.0f;
    e.enmo_mean_g = 0.0f;
    e.stillness_max_s = 30.0f;  // the whole epoch
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(out.probs.drowsy > out.probs.awake);
    TEST_ASSERT_TRUE(out.probs.drowsy > 0.5f);
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::SustainedLowMotion));
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::ProlongedInactivity));
}

void test_heuristic_probabilities_sum_to_one(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    const float low_motion[] = {0.0f, 0.33f, 0.5f, 0.75f, 1.0f};

    for (size_t i = 0; i < sizeof(low_motion) / sizeof(low_motion[0]); ++i) {
        EpochSummary e = makeEpoch();
        e.enmo_low_ratio = low_motion[i];
        e.stillness_max_s = 30.0f * low_motion[i];
        const EpochHistory history = historyFor(e);
        const WakeEstimate out = estimator.estimate(e, history);
        const float sum = out.probs.awake + out.probs.drowsy + out.probs.sleep;
        TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, sum);
        TEST_ASSERT_TRUE(out.probs.awake >= 0.0f && out.probs.awake <= 1.0f);
        TEST_ASSERT_TRUE(out.probs.drowsy >= 0.0f && out.probs.drowsy <= 1.0f);
        TEST_ASSERT_TRUE(out.probs.sleep >= 0.0f && out.probs.sleep <= 1.0f);
    }
}

void test_heuristic_monotonic_in_motion(void) {
    // F06 §9: less motion must never lower the drowsy probability.
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    const float ratios[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

    float previous_drowsy = -1.0f;
    float previous_awake = 2.0f;
    for (size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); ++i) {
        EpochSummary e = makeEpoch();
        e.enmo_low_ratio = ratios[i];
        e.stillness_max_s = 30.0f * ratios[i];
        const EpochHistory history = historyFor(e);
        const WakeEstimate out = estimator.estimate(e, history);
        TEST_ASSERT_TRUE(out.probs.drowsy + kFloatTolerance >= previous_drowsy);
        TEST_ASSERT_TRUE(out.probs.awake <= previous_awake + kFloatTolerance);
        previous_drowsy = out.probs.drowsy;
        previous_awake = out.probs.awake;
    }
}

void test_heuristic_monotonic_in_heart_rate(void) {
    // F06 §9: a lower baseline-relative HR must never lower the drowsy
    // probability (motion held constant).
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    const float z_scores[] = {1.0f, 0.0f, -0.5f, -1.0f, -2.0f};

    float previous_drowsy = -1.0f;
    for (size_t i = 0; i < sizeof(z_scores) / sizeof(z_scores[0]); ++i) {
        EpochSummary e = makeEpoch();
        e.baseline_valid = true;
        e.hr_valid_ratio = 1.0f;
        e.hr_z_baseline = z_scores[i];
        const EpochHistory history = historyFor(e);
        const WakeEstimate out = estimator.estimate(e, history);
        TEST_ASSERT_TRUE(out.probs.drowsy + kFloatTolerance >= previous_drowsy);
        previous_drowsy = out.probs.drowsy;
    }
}

void test_heuristic_invalid_when_imu_insufficient(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.imu_usable = false;
    e.quality = DataQuality::Insufficient;
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.awake);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.sleep);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.confidence);
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::SensorUnavailable));
}

void test_heuristic_works_with_baseline_absent(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.baseline_valid = false;  // F13 not wired / no profile (F06 §3)
    e.hr_valid_ratio = 1.0f;
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(out.probs.valid);  // absence is not an error
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::HrBelowBaseline));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f,
                             out.probs.awake + out.probs.drowsy + out.probs.sleep);
}

void test_heuristic_low_hr_below_baseline_sets_reason(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.baseline_valid = true;
    e.hr_valid_ratio = 1.0f;
    e.hr_z_baseline = -1.0f;  // one MAD below: full term activation
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::HrBelowBaseline));

    e.hr_z_baseline = 0.0f;  // at the baseline: no claim to explain
    const WakeEstimate at_baseline = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(!hasReason(at_baseline.reasons, ReasonCode::HrBelowBaseline));
}

void test_heuristic_sparse_hr_coverage_gives_no_hr_term(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.baseline_valid = true;
    e.hr_valid_ratio = 0.2f;  // 6 of 30 windows: below kMinHrValidRatio
    e.hr_z_baseline = -2.0f;
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::HrBelowBaseline));
}

void test_heuristic_degraded_quality_lowers_confidence(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochSummary good = makeEpoch();
    const WakeEstimate good_out = estimator.estimate(good, historyFor(good));

    EpochSummary degraded = makeEpoch();
    degraded.quality = DataQuality::Degraded;
    const WakeEstimate degraded_out = estimator.estimate(degraded, historyFor(degraded));

    TEST_ASSERT_TRUE(good_out.probs.valid && degraded_out.probs.valid);
    TEST_ASSERT_TRUE(degraded_out.probs.confidence < good_out.probs.confidence);
    TEST_ASSERT_TRUE(degraded_out.probs.confidence <= cfg.base_confidence);
    TEST_ASSERT_TRUE(hasReason(degraded_out.reasons, ReasonCode::LowSignalQuality));
}

void test_heuristic_missing_ppg_flagged_and_discounted(void) {
    // F06 §10: PPG unavailable => IMU-only estimate with reduced confidence,
    // flagged in the output.
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochSummary with_ppg = makeEpoch();
    const WakeEstimate full = estimator.estimate(with_ppg, historyFor(with_ppg));

    EpochSummary no_ppg = makeEpoch();
    no_ppg.ppg_usable = false;
    no_ppg.quality = DataQuality::Degraded;
    no_ppg.hr_valid_ratio = 0.0f;
    const WakeEstimate imu_only = estimator.estimate(no_ppg, historyFor(no_ppg));

    TEST_ASSERT_TRUE(imu_only.probs.valid);  // never an invalid estimate just
                                             // because PPG is gone
    TEST_ASSERT_TRUE(imu_only.probs.confidence < full.probs.confidence);
    TEST_ASSERT_TRUE(hasReason(imu_only.reasons, ReasonCode::SensorUnavailable));
    TEST_ASSERT_TRUE(hasReason(imu_only.reasons, ReasonCode::LowSignalQuality));
}

void test_heuristic_reason_threshold_is_half_evidence(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochSummary below = makeEpoch();
    below.enmo_low_ratio = cfg.reason_activation_threshold - 0.01f;
    WakeEstimate out = estimator.estimate(below, historyFor(below));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::SustainedLowMotion));

    EpochSummary at = makeEpoch();
    at.enmo_low_ratio = cfg.reason_activation_threshold;
    out = estimator.estimate(at, historyFor(at));
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::SustainedLowMotion));
}

void test_heuristic_time_of_day_window(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochSummary in_window = makeEpoch();
    in_window.tod_valid = true;
    in_window.tod_min = 900;  // 15:00, inside 14:00-16:00
    WakeEstimate out = estimator.estimate(in_window, historyFor(in_window));
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::TimeOfDayTypicalDrowsy));

    EpochSummary outside = makeEpoch();
    outside.tod_valid = true;
    outside.tod_min = 660;  // 11:00
    out = estimator.estimate(outside, historyFor(outside));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::TimeOfDayTypicalDrowsy));

    EpochSummary unknown = makeEpoch();
    unknown.tod_valid = false;  // no wall clock: no time-of-day claim
    out = estimator.estimate(unknown, historyFor(unknown));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::TimeOfDayTypicalDrowsy));
}

void test_heuristic_sleep_needs_duration_evidence(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochSummary first = makeEpoch();
    first.enmo_low_ratio = 1.0f;
    first.stillness_max_s = 30.0f;
    const WakeEstimate one_epoch = estimator.estimate(first, historyFor(first));

    // Same epoch, but backed by kSleepLowMotionEpochs of stillness.
    EpochHistory long_history{};
    for (int i = 0; i < cfg.sleep_low_motion_epochs; ++i) {
        EpochSummary still = makeEpoch(static_cast<TimeMs>(i) * 30000u);
        still.enmo_low_ratio = 1.0f;
        still.stillness_max_s = 30.0f;
        pushHistory(long_history, still);
    }
    const EpochSummary* current = long_history.at(long_history.size() - 1);
    TEST_ASSERT_TRUE(current != nullptr);
    const WakeEstimate sustained = estimator.estimate(*current, long_history);

    TEST_ASSERT_TRUE(sustained.probs.sleep > one_epoch.probs.sleep);
    TEST_ASSERT_TRUE(hasReason(sustained.reasons, ReasonCode::SustainedLowMotion));
    TEST_ASSERT_TRUE(hasReason(sustained.reasons, ReasonCode::ProlongedInactivity));
}

void test_heuristic_first_epoch_has_no_trend_terms(void) {
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.enmo_low_ratio = 1.0f;
    const EpochHistory history = historyFor(e);  // no previous epoch at all

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::AbruptMotionDrop));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::HrTrendChange));
}

void test_heuristic_gap_epoch_is_not_a_trend_reference(void) {
    // healthy epoch -> sparse gap epoch -> resume. The gap epoch holds a
    // single window, so it must serve as neither the ENMO-drop nor the
    // HR-trend reference — otherwise a data gap fabricates both trends and
    // F07 would act on evidence nobody observed (F06 §5).
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};

    EpochHistory history{};
    EpochSummary healthy = makeEpoch(30000);
    healthy.enmo_mean_g = 0.10f;
    pushHistory(history, healthy);

    EpochSummary gap = makeEpoch(60000);
    gap.sample_count = 1;  // one window folded in across the gap
    gap.imu_usable = false;
    gap.quality = DataQuality::Insufficient;
    gap.enmo_mean_g = 0.20f;  // would read as a 90 % drop from here
    gap.hr_mean_bpm = 90.0f;  // would read as a 20 bpm fall from here
    gap.baseline_valid = true;
    gap.hr_valid_ratio = 1.0f;
    pushHistory(history, gap);

    EpochSummary resumed = makeEpoch(90000);
    resumed.enmo_mean_g = 0.02f;
    resumed.baseline_valid = true;
    pushHistory(history, resumed);

    const WakeEstimate out = estimator.estimate(resumed, history);
    TEST_ASSERT_TRUE(out.probs.valid);  // the resume epoch itself is fine
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::AbruptMotionDrop));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::HrTrendChange));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, enmoDropRatio(resumed, history));
}

void test_heuristic_insufficient_quality_is_invalid(void) {
    // F06 §8 (kMinInputQuality): "Insufficient inputs ⇒ valid=false". The
    // epoch's quality is the worst over ALL windows while the quorum counts
    // only the usable ones, so a mixed epoch (20 Good + 10 Insufficient)
    // reaches the quorum AND stays Insufficient — and then must not publish
    // probabilities (F07 discards Insufficient estimates).
    const WakeConfig cfg{};
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.imu_usable = true;  // quorum met: 20 usable of 30 windows
    e.quality = DataQuality::Insufficient;
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.probs.input_quality));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.awake);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.sleep);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.confidence);
}

void test_heuristic_non_finite_weights_report_no_estimate(void) {
    // A corrupt config (NaN weight, F18 settings gone wrong) must not turn
    // into NaN probabilities on the wire (F06 §10: never publish bad data).
    WakeConfig cfg{};
    cfg.w_low_motion = std::numeric_limits<float>::quiet_NaN();
    const HeuristicAlertnessEstimator estimator{cfg};
    EpochSummary e = makeEpoch();
    e.enmo_low_ratio = 1.0f;  // would otherwise be full drowsy evidence
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.awake);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.sleep);
    TEST_ASSERT_TRUE(std::isfinite(out.probs.confidence));
}

void test_config_zero_feature_window_reports_no_expectation(void) {
    // Config validation (F06 §10): a zero window length must not divide by
    // zero in expectedSamplesPerEpoch(); it reports "no expectation".
    const WakeConfig defaults{};
    TEST_ASSERT_EQUAL_UINT16(30, expectedSamplesPerEpoch(defaults));

    WakeConfig cfg{};
    cfg.feature_window_ms = 0;  // nonsense config, same class as epoch_length_ms == 0
    TEST_ASSERT_EQUAL_UINT16(0, expectedSamplesPerEpoch(cfg));

    // The confidence path survives it: completeness is unknown -> fill 0,
    // never a crash and never a fabricated ceiling.
    const HeuristicAlertnessEstimator estimator{cfg};
    const EpochSummary e = makeEpoch();
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.confidence);
}
