// test/bw_wake/test_bw_wake/test_wakefulness_estimator.cpp — F06's L2 entry
// point end to end: epoch rolling, estimator choice, startup transient,
// budget (F06 §9, §11).

#include <bw/wake/wake_telemetry.h>
#include <cstdio>
#include <cstring>
#include <limits>

#include <unity.h>

#include "wake_test_support.h"

using namespace bw;
using namespace bw::wake;
using namespace bw::wake::test;

void test_motion_telemetry_integer_conversion_preserves_threshold_and_rejects_bad_values(void) {
    TEST_ASSERT_EQUAL_UINT32(40000, telemetryMicroG(kDemoAwakeMotionEnmoG).value);
    TEST_ASSERT_EQUAL_UINT32(20000, telemetryMicroG(0.02f).value);
    TEST_ASSERT_EQUAL_UINT32(0, telemetryMicroG(0.0f).value);
    TEST_ASSERT_EQUAL_UINT32(1250000, telemetryMicroG(1.25f).value);
    TEST_ASSERT_FALSE(telemetryMicroG(-1.0f).ok());
    TEST_ASSERT_FALSE(telemetryMicroG(std::numeric_limits<float>::quiet_NaN()).ok());
    TEST_ASSERT_FALSE(telemetryMicroG(std::numeric_limits<float>::infinity()).ok());
    TEST_ASSERT_FALSE(telemetryMicroG(std::numeric_limits<float>::max()).ok());
}

namespace {

// A window of someone lying still with a slow heart — the scenario F06 is
// supposed to read as drowsy.
WakeSample restingSample(TimeMs t_ms, uint16_t window, float hr_bpm) {
    WakeSample s = makeSample(t_ms, 0.001f);
    // stillness_ms is continuous time below the motion threshold, so it
    // grows window by window inside one epoch.
    s.motion.stillness_ms = static_cast<uint32_t>(window + 1) * 1000u;
    s.ppg.hr_bpm = hr_bpm;
    s.activity.context = ActivityContext::LyingResting;
    s.activity.confidence = 0.9f;
    return s;
}

WakeSample movingSample(TimeMs t_ms, float hr_bpm) {
    WakeSample s = makeSample(t_ms, 0.2f);
    s.ppg.hr_bpm = hr_bpm;
    s.activity.context = ActivityContext::Walking;
    s.activity.confidence = 0.9f;
    return s;
}

FakeBaseline restingBaseline(float resting_hr_bpm) {
    FakeBaseline baseline{};
    baseline.available = true;
    baseline.p.valid = true;
    baseline.p.resting_hr_bpm = resting_hr_bpm;
    baseline.p.resting_hr_mad = 5.0f;
    return baseline;
}

WakeConfig epochOnlyConfig() {
    WakeConfig cfg{};
    cfg.low_motion_enmo_g = 0.3f;  // [TEST] Keep synthetic 0.2 g epoch fixtures off the fast lane.
    cfg.demo_awake_motion_enmo_g = cfg.low_motion_enmo_g;
    return cfg;
}

}  // namespace

void test_input_gate_reproduces_phased_window_end_latency(void) {
    // [DERIVED] Independent 1 Hz stages can consume a window almost
    // two seconds after its END: F02 publication lag plus F06 consumer lag.
    // The extra 33 ms mirrors the handoff's estimated 2033 ms age; it is a
    // synthetic reproduction, not a measurement of the device rejection.
    constexpr TimeMs kPublicationLagMs = 1000;
    constexpr TimeMs kConsumerPhaseMs = 1033;
    constexpr TimeMs kBootOffsetMs = TimeMs{1} << 32;
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    for (unsigned window = 1; window <= 4; ++window) {
        const TimeMs window_end_ms = kBootOffsetMs + window * kFeatureWindowMs;
        WakeSample sample = makeSample(window_end_ms, 0.05f);
        sample.t_ms = window_end_ms + kPublicationLagMs + kConsumerPhaseMs;
        const WearEstimate wear{sample.t_ms - kFeatureWindowMs, WearState::Worn, 1.0f};
        const auto out = estimator.step(sample, false, 0, &wear);
        TEST_ASSERT_TRUE(out.probs.valid);
        TEST_ASSERT_TRUE(out.probs.wake_motion.valid);
        TEST_ASSERT_TRUE(out.probs.wake_motion.qualifying);
        TEST_ASSERT_EQUAL_UINT64(window_end_ms, out.probs.wake_motion.t_ms);
        TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::None),
                          static_cast<unsigned>(estimator.lastInputRejection()));
        TEST_ASSERT_EQUAL_STRING("none", inputRejectionName(estimator.lastInputRejection()));
    }
}

void test_input_gate_motion_age_boundary_preserves_original_evidence_stamp(void) {
    // [DERIVED] Check exactly the existing limit and one millisecond outside.
    const TimeMs ages[] = {kDemoMotionFreshnessMs, kDemoMotionFreshnessMs + 1};
    for (const TimeMs age : ages) {
        WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
        WakeSample sample = makeSample(10000, 0.05f);
        sample.t_ms += age;
        const WearEstimate wear{sample.t_ms, WearState::Worn, 1.0f};
        const auto out = estimator.step(sample, false, 0, &wear);
        if (age == kDemoMotionFreshnessMs) {
            TEST_ASSERT_TRUE(out.probs.valid);
            TEST_ASSERT_TRUE(out.probs.wake_motion.valid);
            TEST_ASSERT_TRUE(out.probs.wake_motion.qualifying);
            TEST_ASSERT_EQUAL_UINT64(sample.motion.t_ms, out.probs.wake_motion.t_ms);
            TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::None),
                              static_cast<unsigned>(estimator.lastInputRejection()));
        } else {
            TEST_ASSERT_FALSE(out.probs.wake_motion.valid);
            TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::MotionStale),
                              static_cast<unsigned>(estimator.lastInputRejection()));
        }
    }
}

void test_input_gate_response_age_does_not_inherit_motion_pipeline_budget(void) {
    const TimeMs ages[] = {kDemoResponseFreshnessMs, kDemoResponseFreshnessMs + 1};
    for (const TimeMs age : ages) {
        WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
        const WakeSample sample = makeSample(10000, 0.001f);
        const auto out = estimator.step(sample, true, sample.t_ms - age);
        TEST_ASSERT_EQUAL(age == kDemoResponseFreshnessMs, out.probs.valid);
        TEST_ASSERT_TRUE(out.probs.wake_motion.valid);
        TEST_ASSERT_FALSE(out.probs.wake_motion.qualifying);
    }
    TEST_ASSERT_EQUAL_UINT32(1500, kDemoWearFreshnessMs);
    TEST_ASSERT_EQUAL_UINT32(1500, kDemoInputGapMs);
}

void test_input_gate_distinguishes_wear_quality_and_invalid_motion(void) {
    const WakeInputRejection cases[] = {
        WakeInputRejection::WearUnsafe,      WakeInputRejection::WearFuture,
        WakeInputRejection::WearStale,       WakeInputRejection::MotionFuture,
        WakeInputRejection::ImuInsufficient, WakeInputRejection::MotionInsufficient,
        WakeInputRejection::EnmoInvalid,
    };
    for (const auto expected : cases) {
        WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
        WakeSample sample = makeSample(10000, 0.05f);
        WearEstimate wear{sample.t_ms, WearState::Worn, 1.0f};
        switch (expected) {
        case WakeInputRejection::WearUnsafe:
            wear.state = WearState::NotWorn;
            break;
        case WakeInputRejection::WearFuture:
            wear.t_ms = sample.t_ms + 1;
            break;
        case WakeInputRejection::WearStale:
            wear.t_ms = sample.t_ms - kDemoWearFreshnessMs - 1;
            break;
        case WakeInputRejection::MotionFuture:
            sample.motion.t_ms = sample.t_ms + 1;
            break;
        case WakeInputRejection::ImuInsufficient:
            sample.quality.imu = DataQuality::Insufficient;
            break;
        case WakeInputRejection::MotionInsufficient:
            sample.motion.quality = DataQuality::Insufficient;
            break;
        case WakeInputRejection::EnmoInvalid:
            sample.motion.enmo_g = std::numeric_limits<float>::quiet_NaN();
            break;
        default:
            TEST_FAIL_MESSAGE("unhandled test case");
            break;
        }
        const auto out = estimator.step(sample, false, 0, &wear);
        TEST_ASSERT_FALSE(out.probs.valid);
        TEST_ASSERT_FALSE(out.probs.wake_motion.valid);
        TEST_ASSERT_EQUAL(static_cast<unsigned>(expected),
                          static_cast<unsigned>(estimator.lastInputRejection()));
        TEST_ASSERT_NOT_EQUAL(0, std::strcmp("unknown", inputRejectionName(expected)));
        estimator.reset();
        TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::None),
                          static_cast<unsigned>(estimator.lastInputRejection()));
    }
}

void test_input_gate_separates_cadence_and_missing_epoch_from_stale_motion(void) {
    WakefulnessEstimator estimator{epochOnlyConfig(), nullptr, nullptr, nullptr};
    const WakeSample first = makeSample(10000, 0.001f);
    const auto startup = estimator.step(first);
    TEST_ASSERT_FALSE(startup.probs.valid);  // no epoch, not an input rejection
    TEST_ASSERT_TRUE(startup.probs.wake_motion.valid);
    TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::None),
                      static_cast<unsigned>(estimator.lastInputRejection()));
    TEST_ASSERT_FALSE(estimator.step(first).probs.wake_motion.valid);
    TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::SampleNotIncreasing),
                      static_cast<unsigned>(estimator.lastInputRejection()));
    const TimeMs after_gap_ms = first.t_ms + kDemoInputGapMs + 1;
    TEST_ASSERT_FALSE(estimator.step(makeSample(after_gap_ms, 0.001f)).probs.wake_motion.valid);
    TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::CadenceGap),
                      static_cast<unsigned>(estimator.lastInputRejection()));
    TEST_ASSERT_TRUE(estimator.step(makeSample(after_gap_ms + kFeatureWindowMs, 0.001f))
                         .probs.wake_motion.valid);
    TEST_ASSERT_EQUAL(static_cast<unsigned>(WakeInputRejection::None),
                      static_cast<unsigned>(estimator.lastInputRejection()));
}

void test_estimator_no_estimate_before_first_epoch(void) {
    const WakeConfig cfg = epochOnlyConfig();
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, nullptr};

    for (uint16_t i = 0; i < 30; ++i) {
        const WakeEstimate out = estimator.step(makeSample(i * 1000u, 0.2f));
        TEST_ASSERT_FALSE(out.probs.valid);  // startup transient, not a guess
    }
}

void test_estimator_emits_at_boundary_and_holds_within_epoch(void) {
    const WakeConfig cfg = epochOnlyConfig();
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, nullptr};

    WakeEstimate last{};
    for (uint16_t i = 0; i < 30; ++i) {
        last = estimator.step(makeSample(i * 1000u, 0.2f));
    }
    last = estimator.step(makeSample(30000, 0.2f));  // commits epoch 0
    TEST_ASSERT_TRUE(last.probs.valid);
    TEST_ASSERT_EQUAL_UINT64(29000, last.probs.t_ms);

    // Inside the next epoch the last decision stands (F06 D2).
    const WakeEstimate held = estimator.step(makeSample(31000, 0.2f));
    TEST_ASSERT_TRUE(held.probs.valid);
    TEST_ASSERT_EQUAL_UINT64(last.probs.t_ms, held.probs.t_ms);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, last.probs.drowsy, held.probs.drowsy);
}

void test_estimator_moving_reads_awake_resting_reads_drowsy(void) {
    const WakeConfig cfg{};
    const FakeBaseline baseline = restingBaseline(60.0f);
    WakefulnessEstimator estimator{cfg, nullptr, &baseline, nullptr};

    WakeEstimate moving{};
    for (uint16_t i = 0; i < 31; ++i) {
        moving = estimator.step(movingSample(i * 1000u, 78.0f));
    }
    TEST_ASSERT_TRUE(moving.probs.valid);
    TEST_ASSERT_TRUE(moving.probs.awake > moving.probs.drowsy);

    estimator.reset();
    WakeEstimate resting{};
    for (uint16_t i = 0; i < 31; ++i) {
        resting = estimator.step(restingSample(30000 + i * 1000u, i, 55.0f));
    }
    TEST_ASSERT_TRUE(resting.probs.valid);
    TEST_ASSERT_TRUE(resting.probs.drowsy > resting.probs.awake);
    TEST_ASSERT_TRUE(resting.probs.drowsy > moving.probs.drowsy);
}

void test_estimator_reset_clears_history_and_answer(void) {
    const WakeConfig cfg = epochOnlyConfig();
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, nullptr};
    for (uint16_t i = 0; i < 31; ++i) {
        static_cast<void>(estimator.step(makeSample(i * 1000u, 0.2f)));
    }
    TEST_ASSERT_TRUE(estimator.assembler().history().size() > 0);

    estimator.reset();
    TEST_ASSERT_EQUAL_UINT16(0, estimator.assembler().history().size());
    TEST_ASSERT_FALSE(estimator.step(makeSample(0, 0.2f)).probs.valid);
}

void test_estimator_falls_back_to_heuristic_when_model_fails(void) {
    const WakeConfig cfg = epochOnlyConfig();
    FakeModel model{};
    model.predict_err = Err::InvalidArg;  // schema mismatch (FEATURE_SCHEMA §3)
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, &model};

    WakeEstimate out{};
    for (uint16_t i = 0; i < 31; ++i) {
        out = estimator.step(movingSample(i * 1000u, 70.0f));
    }
    // 00 §5 graceful degradation: the answer and its reasons come from the
    // same scorer, so a failed model never leaves an empty estimate.
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_EQUAL_UINT32(1, model.predict_calls);
}

void test_estimator_uses_model_when_it_answers(void) {
    const WakeConfig cfg = epochOnlyConfig();
    FakeModel model{};
    model.output.n_classes = 3;
    model.output.p[0] = 0.25f;
    model.output.p[1] = 0.50f;
    model.output.p[2] = 0.25f;
    model.output.confidence = 0.7f;
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, &model};

    WakeEstimate out{};
    for (uint16_t i = 0; i < 31; ++i) {
        out = estimator.step(movingSample(i * 1000u, 70.0f));
    }
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.50f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_UINT32(1, model.predict_calls);
}

void test_estimator_sparse_epoch_reports_no_estimate(void) {
    // Windows stop arriving: only 3 windows land in the epoch, below
    // kMinSamplesPerEpoch. The next epoch commits it and F06 says "no
    // estimate" rather than extrapolating (F06 §5).
    WakeConfig cfg = epochOnlyConfig();
    cfg.epoch_length_ms = 30000;
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, nullptr};

    for (uint16_t i = 0; i < 3; ++i) {
        TEST_ASSERT_FALSE(estimator.step(makeSample(i * 1000u, 0.2f)).probs.valid);
    }
    const WakeEstimate sparse = estimator.step(makeSample(30000, 0.2f));
    TEST_ASSERT_FALSE(sparse.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sparse.probs.drowsy);
}

void test_estimator_mixed_window_quality_reports_no_estimate(void) {
    // 20 Good windows then 10 Insufficient ones: the quorum is met (20 >=
    // kMinSamplesPerEpoch) so imu_usable stays true, but the epoch's honest
    // quality is Insufficient (worst over ALL windows) — and F06 §8 says
    // Insufficient ⇒ valid=false, which is exactly what F07 relies on.
    const WakeConfig cfg = epochOnlyConfig();
    WakefulnessEstimator estimator{cfg, nullptr, nullptr, nullptr};

    WakeEstimate out{};
    for (uint16_t i = 0; i < 30; ++i) {
        const DataQuality q = (i < 20) ? DataQuality::Good : DataQuality::Insufficient;
        out = estimator.step(makeSample(i * 1000u, 0.2f, q));
    }
    out = estimator.step(makeSample(30000, 0.2f));  // commits epoch 0

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.probs.input_quality));
    TEST_ASSERT_FALSE(out.probs.valid);  // the invariant under audit
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.awake);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.sleep);

    // The held answer inside the next epoch stays invalid too (F06 D2).
    const WakeEstimate held = estimator.step(makeSample(31000, 0.2f));
    TEST_ASSERT_FALSE(held.probs.valid);
    TEST_ASSERT_EQUAL_UINT64(29000, held.probs.t_ms);
}

void test_fast_lane_requires_burst_then_emits_each_low_motion_tick(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    TEST_ASSERT_FALSE(estimator.step(makeSample(1000, 0.01f)).probs.valid);
    WakeEstimate burst = estimator.step(makeSample(2000, kDemoStrongMotionEnmoG));
    TEST_ASSERT_TRUE(burst.probs.valid);
    TEST_ASSERT_TRUE(burst.probs.awake > burst.probs.drowsy);
    WakeEstimate still = estimator.step(makeSampleNoPpg(3000, 0.01f));
    TEST_ASSERT_TRUE(still.probs.valid);
    TEST_ASSERT_TRUE(still.probs.drowsy > still.probs.awake);
    TEST_ASSERT_EQUAL_UINT64(3000, still.probs.t_ms);
    TEST_ASSERT_TRUE(still.probs.confidence < kConfidenceGoodFactor);
    const ReasonMask short_burst = static_cast<ReasonMask>(1)
                                   << static_cast<uint8_t>(ReasonCode::ShortMotionBurst);
    TEST_ASSERT_TRUE((still.reasons & short_burst) != 0);
    still = estimator.step(makeSampleNoPpg(4000, 0.01f));
    TEST_ASSERT_EQUAL_UINT64(4000, still.probs.t_ms);
    TEST_ASSERT_TRUE(still.probs.drowsy > still.probs.awake);
    TEST_ASSERT_TRUE((still.reasons & short_burst) == 0);
}

void test_fast_lane_sub_burst_motion_publishes_and_holds_awake(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    WakeEstimate awake = estimator.step(makeSampleNoPpg(1000, 0.08f));
    TEST_ASSERT_TRUE(awake.probs.valid);
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    awake = estimator.step(makeSampleNoPpg(2000, 0.01f));
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    static_cast<void>(estimator.step(makeSample(3000, kDemoStrongMotionEnmoG)));
    const WakeEstimate drowsy = estimator.step(makeSample(4000, 0.01f));
    TEST_ASSERT_TRUE(drowsy.probs.drowsy > drowsy.probs.awake);
}

void test_fast_lane_motion_resets_to_awake_and_check_holds_until_burst(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    static_cast<void>(estimator.step(makeSample(1000, kDemoStrongMotionEnmoG)));
    WakeEstimate drowsy = estimator.step(makeSample(2000, 0.01f));
    TEST_ASSERT_TRUE(drowsy.probs.drowsy > drowsy.probs.awake);
    WakeEstimate awake = estimator.step(makeSampleNoPpg(3000, 0.08f));
    TEST_ASSERT_TRUE(awake.probs.valid);
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    awake = estimator.step(makeSample(4000, 0.01f));
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    awake = estimator.step(makeSample(5000, 0.01f), true, 4500);
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    awake = estimator.step(makeSample(6000, 0.01f), true, 4500);
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    static_cast<void>(estimator.step(makeSample(7000, kDemoStrongMotionEnmoG), true, 4500));
    const WakeEstimate retriggered = estimator.step(makeSample(8000, 0.01f), true, 4500);
    TEST_ASSERT_TRUE(retriggered.probs.drowsy > retriggered.probs.awake);
}

void test_fast_lane_stale_or_gapped_imu_is_invalid_and_epoch_cannot_override(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    static_cast<void>(estimator.step(makeSample(1000, kDemoStrongMotionEnmoG)));
    WakeEstimate drowsy = estimator.step(makeSample(2000, 0.01f));
    TEST_ASSERT_TRUE(drowsy.probs.drowsy > drowsy.probs.awake);
    WakeSample stale = makeSample(3000, 0.01f);
    stale.motion.t_ms = 0;
    WakeEstimate invalid = estimator.step(stale);
    TEST_ASSERT_FALSE(invalid.probs.valid);
    WakefulnessEstimator gap_estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    static_cast<void>(gap_estimator.step(makeSample(1000, kDemoStrongMotionEnmoG)));
    invalid = gap_estimator.step(makeSample(3000, 0.01f));
    TEST_ASSERT_FALSE(invalid.probs.valid);
    invalid = gap_estimator.step(makeSample(4000, 0.01f));
    TEST_ASSERT_FALSE(invalid.probs.valid);  // a time gap clears the armed trigger

    WakefulnessEstimator insufficient_estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    static_cast<void>(insufficient_estimator.step(makeSample(1000, kDemoStrongMotionEnmoG)));
    WakeSample unavailable_imu = makeSample(2000, 0.01f, DataQuality::Insufficient);
    invalid = insufficient_estimator.step(unavailable_imu);
    TEST_ASSERT_FALSE(invalid.probs.valid);

    WakefulnessEstimator boundary_estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    static_cast<void>(boundary_estimator.step(makeSample(1000, kDemoStrongMotionEnmoG)));
    WakeEstimate boundary{};
    for (TimeMs t = 2000; t <= 30000; t += 1000) {
        boundary = boundary_estimator.step(makeSample(t, 0.01f));
    }
    TEST_ASSERT_TRUE(boundary.probs.drowsy > boundary.probs.awake);
    TEST_ASSERT_EQUAL_UINT64(30000, boundary.probs.t_ms);
}

void test_wake_sizes_within_budget(void) {
    // F06 §11: RAM < 4 KB. The estimator owns the 10-epoch ring, the config,
    // the accumulators and the last answer — this is its static footprint
    // (stack use is measured on device at the DoD gate).
    TEST_ASSERT_TRUE(sizeof(EpochSummary) <= 80);
    // CR-034 adds 16 bytes of timestamped motion evidence to the old 48-byte budget.
    TEST_ASSERT_TRUE(sizeof(WakeEstimate) <= 64);
    TEST_ASSERT_TRUE(sizeof(WakefulnessEstimator) <= 4096);

    // Report the measured numbers so the status log quotes facts, not bounds.
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "sizeof: EpochSummary=%u B, WakeEstimate=%u B, "
                  "WakefulnessEstimator=%u B (limits 80/64/4096)",
                  static_cast<unsigned>(sizeof(EpochSummary)),
                  static_cast<unsigned>(sizeof(WakeEstimate)),
                  static_cast<unsigned>(sizeof(WakefulnessEstimator)));
    TEST_MESSAGE(msg);
}

void test_demo_recent_awake_expires_and_bad_motion_clears_hold(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    (void)estimator.step(makeSample(1000, 0.08f));
    for (TimeMs t = 2000; t < 1000 + kDemoAwakeEvidenceMs; t += 1000) {
        TEST_ASSERT_TRUE(estimator.step(makeSample(t, 0.01f)).probs.valid);
    }
    // Before any epoch exists, an expired response cannot fabricate Awake.
    TEST_ASSERT_FALSE(estimator.step(makeSample(1000 + kDemoAwakeEvidenceMs, 0.01f)).probs.valid);
    (void)estimator.step(makeSample(7000, 0.08f));
    TEST_ASSERT_FALSE(
        estimator.step(makeSample(8000, 0.01f, DataQuality::Insufficient)).probs.valid);
    TEST_ASSERT_FALSE(estimator.step(makeSample(9000, 0.01f)).probs.valid);
}

void test_demo_motion_reasons_reach_contract_and_notworn_resets_burst(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    const WakeEstimate burst = estimator.step(makeSample(1000, kDemoStrongMotionEnmoG));
    const ReasonMask marker = ReasonMask{1} << static_cast<uint8_t>(ReasonCode::ShortMotionBurst);
    TEST_ASSERT_BITS(marker, marker, burst.probs.reasons);
    const WakeEstimate still = estimator.step(makeSample(2000, 0.01f));
    TEST_ASSERT_EQUAL_UINT32(still.reasons, still.probs.reasons);
    WearEstimate wear{};
    wear.t_ms = 3000;
    wear.state = WearState::NotWorn;
    TEST_ASSERT_FALSE(estimator.step(makeSample(3000, 0.01f), true, 2500, &wear).probs.valid);
    wear.t_ms = 4000;
    wear.state = WearState::Worn;
    const auto rewear = estimator.step(makeSample(4000, 0.01f), true, 2500, &wear);
    TEST_ASSERT_TRUE(rewear.probs.valid);  // bounded initial-contact grace
    TEST_ASSERT_TRUE(rewear.probs.awake > rewear.probs.drowsy);
    TEST_ASSERT_EQUAL_UINT32(0, rewear.probs.reasons & marker);
}

void test_demo_initial_contact_grace_hands_off_to_background_epoch_without_gap(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    WearEstimate wear{};
    wear.state = WearState::Worn;
    WakeEstimate out{};
    for (TimeMs t = 1000; t <= 32000; t += 1000) {
        wear.t_ms = t;
        out = estimator.step(makeSample(t, 0.01f), false, 0, &wear);
        TEST_ASSERT_TRUE(out.probs.valid);
    }
    // After the bounded grace the epoch's original stamp, not 'now', is kept.
    TEST_ASSERT_TRUE(out.probs.t_ms < 32000);
}

void test_demo_gentle_motion_wakes_without_arming_strong_burst(void) {
    // [EXPERIMENTAL] 0.05 g is above the user-selected 0.04 g demo threshold
    // and remains below the separate 0.35 g strong-burst threshold.
    constexpr float kGentleMotionEnmoG = 0.05f;
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    const auto awake = estimator.step(makeSampleNoPpg(1000, kGentleMotionEnmoG));
    TEST_ASSERT_TRUE(awake.probs.valid);
    TEST_ASSERT_TRUE(awake.probs.awake > awake.probs.drowsy);
    const ReasonMask burst = ReasonMask{1} << static_cast<uint8_t>(ReasonCode::ShortMotionBurst);
    TEST_ASSERT_EQUAL_UINT32(0, awake.probs.reasons & burst);
    for (TimeMs t = 2000; t < 1000 + kDemoAwakeEvidenceMs; t += 1000) {
        const auto held = estimator.step(makeSampleNoPpg(t, 0.001f));
        TEST_ASSERT_TRUE(held.probs.awake > held.probs.drowsy);
        TEST_ASSERT_EQUAL_UINT32(0, held.probs.reasons & burst);
    }
    TEST_ASSERT_FALSE(
        estimator.step(makeSampleNoPpg(1000 + kDemoAwakeEvidenceMs, 0.001f)).probs.valid);
}

void test_demo_gentle_motion_threshold_boundary_and_override(void) {
    WakeConfig cfg;
    WakefulnessEstimator boundary{cfg, nullptr, nullptr, nullptr};
    TEST_ASSERT_FALSE(boundary.step(makeSampleNoPpg(1000, kDemoAwakeMotionEnmoG)).probs.valid);
    TEST_ASSERT_FALSE(boundary.step(makeSampleNoPpg(2000, kDemoAwakeMotionEnmoG)).probs.valid);
    TEST_ASSERT_TRUE(
        boundary.step(makeSampleNoPpg(3000, kDemoAwakeMotionEnmoG + 0.001f)).probs.valid);
    cfg.demo_awake_motion_enmo_g = kDemoAwakeMotionEnmoG + 0.01f;
    WakefulnessEstimator original{cfg, nullptr, nullptr, nullptr};
    TEST_ASSERT_FALSE(
        original.step(makeSampleNoPpg(1000, kDemoAwakeMotionEnmoG + 0.001f)).probs.valid);
}

void test_demo_motion_evidence_is_current_not_held_awake(void) {
    WakefulnessEstimator estimator{WakeConfig{}, nullptr, nullptr, nullptr};
    auto out = estimator.step(makeSampleNoPpg(1000, 0.05f));
    TEST_ASSERT_TRUE(out.probs.wake_motion.valid);
    TEST_ASSERT_TRUE(out.probs.wake_motion.qualifying);
    TEST_ASSERT_EQUAL_UINT64(1000, out.probs.wake_motion.t_ms);
    out = estimator.step(makeSampleNoPpg(2000, kDemoAwakeMotionEnmoG));
    TEST_ASSERT_TRUE(out.probs.awake > out.probs.drowsy);
    TEST_ASSERT_TRUE(out.probs.wake_motion.valid);
    TEST_ASSERT_FALSE(out.probs.wake_motion.qualifying);
    TEST_ASSERT_EQUAL_UINT64(2000, out.probs.wake_motion.t_ms);
    out = estimator.step(makeSampleNoPpg(5000, 0.05f));
    TEST_ASSERT_FALSE(out.probs.wake_motion.valid);  // gap
    auto sample = makeSampleNoPpg(6000, 0.05f);
    sample.motion.t_ms = 5500;
    out = estimator.step(sample);
    TEST_ASSERT_EQUAL_UINT64(5500, out.probs.wake_motion.t_ms);
    WearEstimate wear{};
    wear.t_ms = 7000;
    wear.state = WearState::NotWorn;
    TEST_ASSERT_FALSE(
        estimator.step(makeSampleNoPpg(7000, 0.05f), false, 0, &wear).probs.wake_motion.valid);
}
