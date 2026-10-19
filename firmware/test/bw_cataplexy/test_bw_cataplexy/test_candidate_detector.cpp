// firmware/test/bw_cataplexy/test_bw_cataplexy/test_candidate_detector.cpp —
// F09 §9 rule-trigger fixtures: 'abrupt drop' vs 'sitting down' vs 'fall',
// plus the failure behaviour of §10.

#include <cstdio>
#include <limits>

#include <unity.h>

#include <bw/cataplexy/candidate_detector.h>
#include <bw/cataplexy/cataplexy_config.h>

#include "fixtures.h"

namespace {

using bw::MotionFeatures;
using bw::PpgFeatures;
using bw::StateSnapshot;
using bw::TimeMs;
using bw::cataplexy::CandidateOutput;
using bw::cataplexy::CataplexyConfig;
using bw::cataplexy::Confounder;
using bw::cataplexy::DetectorInput;
using bw::cataplexy::EventCandidateDetector;
using bw::cataplexy::kBaseCandidateConfidence;
using bw::cataplexy::kDropHistoryWindows;
using bw::cataplexy::kMinCandidateConfidence;
using bw::cataplexy::reasonBit;
using bw::cataplexy::testing::DropScenario;
using bw::cataplexy::testing::DropTimeline;
using bw::cataplexy::testing::hasConfounder;
using bw::cataplexy::testing::makeDropTimeline;
using bw::cataplexy::testing::makeMotion;
using bw::cataplexy::testing::makePpg;
using bw::cataplexy::testing::makeState;
using bw::cataplexy::testing::runDrop;
using bw::cataplexy::testing::stepWindow;

// One window without PPG: the source is absent (02 §5), so the evidence is
// IMU-only and no heart-rate corroboration can ever apply.
CandidateOutput stepImuOnly(EventCandidateDetector& detector, TimeMs t_ms,
                            const MotionFeatures& motion, const StateSnapshot& state) {
    DetectorInput input;
    input.t_ms = t_ms;
    input.motion = &motion;
    input.state = &state;
    return detector.step(input);
}

}  // namespace

void test_detector_disabled_when_imu_absent(void) {
    EventCandidateDetector detector;
    DetectorInput input;
    input.t_ms = 1000;  // motion, ppg and state all absent (spec §10: IMU failed)

    const CandidateOutput out = detector.step(input);

    TEST_ASSERT_TRUE(out.disabled);
    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::SensorUnavailable), out.reasons);
}

void test_detector_no_snapshot_evaluates_nothing(void) {
    EventCandidateDetector detector;
    const MotionFeatures motion = makeMotion(1000, 0.25f, 30.0f, 0.5f);

    DetectorInput input;
    input.t_ms = 1000;
    input.motion = &motion;

    const CandidateOutput out = detector.step(input);

    TEST_ASSERT_FALSE(out.disabled);
    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
    TEST_ASSERT_EQUAL_UINT32(0, out.reasons);
    TEST_ASSERT_EQUAL_UINT32(0, detector.historyDepth());  // unworn/unknown data kept nothing
}

void test_detector_first_call_is_empty(void) {
    EventCandidateDetector detector;
    const MotionFeatures motion = makeMotion(1000, 0.25f, 30.0f, 0.5f);
    const PpgFeatures ppg = makePpg(1000, 0.0f, 0.0f, false);
    const StateSnapshot state = makeState(1000);

    const CandidateOutput out = stepWindow(detector, 1000, motion, ppg, state);

    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
    TEST_ASSERT_EQUAL_UINT32(0, out.reasons);
    TEST_ASSERT_EQUAL_UINT32(1, detector.historyDepth());
}

void test_detector_clean_drop_raises(void) {
    EventCandidateDetector detector;
    const CandidateOutput out = runDrop(detector, DropScenario::Clean);

    TEST_ASSERT_TRUE(out.raised);
    TEST_ASSERT_FALSE(out.disabled);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence, out.confidence);
    TEST_ASSERT_TRUE(out.confidence >= kMinCandidateConfidence);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::AbruptMotionDrop), out.reasons);
    TEST_ASSERT_EQUAL_UINT8(0, out.confounders);
}

void test_detector_sitting_down_is_not_raised(void) {
    EventCandidateDetector detector;
    const CandidateOutput out = runDrop(detector, DropScenario::SittingDown);

    TEST_ASSERT_TRUE(hasConfounder(out.confounders, Confounder::PostureChange));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 0.5f, out.confidence);
    TEST_ASSERT_FALSE(out.raised);  // below kMinCandidateConfidence -> research stream only
    TEST_ASSERT_TRUE(out.confidence > 0.0f);
    TEST_ASSERT_TRUE(out.reasons != 0);  // §9: confidence never without reason codes
}

void test_detector_fall_is_not_raised(void) {
    EventCandidateDetector detector;
    const CandidateOutput out = runDrop(detector, DropScenario::Fall);

    TEST_ASSERT_TRUE(hasConfounder(out.confounders, Confounder::FallLike));
    TEST_ASSERT_FALSE(hasConfounder(out.confounders, Confounder::PostureChange));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 0.4f, out.confidence);
    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_TRUE(out.reasons != 0);
}

void test_detector_brief_burst_is_not_raised(void) {
    EventCandidateDetector detector;
    const CandidateOutput out = runDrop(detector, DropScenario::BriefBurst);

    TEST_ASSERT_TRUE(hasConfounder(out.confounders, Confounder::BriefBurst));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 0.5f, out.confidence);
    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_TRUE(out.reasons != 0);
}

void test_detector_hr_corroborates_clean_drop(void) {
    EventCandidateDetector detector;
    const CandidateOutput out = runDrop(detector, DropScenario::HrDrop);

    TEST_ASSERT_TRUE(out.raised);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::HrTrendChange), out.reasons);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 1.15f, out.confidence);
}

void test_detector_hr_does_not_raise_a_confounder(void) {
    // Same PPG corroboration as the HrDrop fixture, but on a posture-change
    // candidate: spec §8 says the ×0.5 penalty must land below the raise
    // threshold on its own, so kHrCorroborationFactor is skipped (0.75 × 1.15
    // × 0.5 would otherwise be 0.43125 >= kMinCandidateConfidence).
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::SittingDown);
    timeline.ppg_earlier = makePpg(timeline.earlier.t_ms, 72.0f, 0.9f, true);
    timeline.ppg_previous = makePpg(timeline.previous.t_ms, 72.0f, 0.9f, true);
    timeline.ppg_current = makePpg(timeline.current.t_ms, 60.0f, 0.9f, true);

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);
    const CandidateOutput out = stepWindow(detector, timeline.current.t_ms, timeline.current,
                                           timeline.ppg_current, timeline.state);

    TEST_ASSERT_TRUE(hasConfounder(out.confounders, Confounder::PostureChange));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 0.5f, out.confidence);
    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_BITS_LOW(reasonBit(bw::ReasonCode::HrTrendChange), out.reasons);
    TEST_ASSERT_TRUE(out.reasons != 0);
}

void test_detector_absent_ppg_evaluates_imu_only(void) {
    // ppg == nullptr with a snapshot present: the evidence is IMU-only, so the
    // drop still raises at the base confidence, PPG quality reads as absent
    // (02 §4.1) and no heart-rate corroboration can apply (00 §1: no data, no
    // claim — but the IMU evidence itself stays valid).
    EventCandidateDetector detector;
    const DropTimeline timeline = makeDropTimeline(DropScenario::HrDrop);

    stepImuOnly(detector, timeline.earlier.t_ms, timeline.earlier, timeline.state);
    stepImuOnly(detector, timeline.previous.t_ms, timeline.previous, timeline.state);
    const CandidateOutput out =
        stepImuOnly(detector, timeline.current.t_ms, timeline.current, timeline.state);

    TEST_ASSERT_FALSE(out.disabled);
    TEST_ASSERT_TRUE(out.raised);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence, out.confidence);
    TEST_ASSERT_BITS_LOW(reasonBit(bw::ReasonCode::HrTrendChange), out.reasons);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::DataQuality::Insufficient),
                            static_cast<uint8_t>(out.quality.ppg));
}

void test_detector_low_confidence_hr_is_ignored(void) {
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::HrDrop);
    // A motion-corrupted beat estimate must never corroborate a motion event.
    timeline.ppg_earlier.hr_confidence = 0.2f;
    timeline.ppg_previous.hr_confidence = 0.2f;
    timeline.ppg_current.hr_confidence = 0.2f;

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);
    const CandidateOutput out = stepWindow(detector, timeline.current.t_ms, timeline.current,
                                           timeline.ppg_current, timeline.state);

    TEST_ASSERT_TRUE(out.raised);
    TEST_ASSERT_BITS_LOW(reasonBit(bw::ReasonCode::HrTrendChange), out.reasons);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence, out.confidence);
}

void test_detector_insufficient_quality_gates(void) {
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::Clean);

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);

    MotionFeatures dead = timeline.current;
    dead.quality = bw::DataQuality::Insufficient;
    const CandidateOutput out =
        stepWindow(detector, dead.t_ms, dead, timeline.ppg_current, timeline.state);

    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::LowSignalQuality), out.reasons);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::DataQuality::Insufficient),
                            static_cast<uint8_t>(out.quality.imu));
}

void test_detector_degraded_quality_lowers_confidence(void) {
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::Clean);
    timeline.earlier.quality = bw::DataQuality::Degraded;
    timeline.previous.quality = bw::DataQuality::Degraded;
    timeline.current.quality = bw::DataQuality::Degraded;

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);
    const CandidateOutput out = stepWindow(detector, timeline.current.t_ms, timeline.current,
                                           timeline.ppg_current, timeline.state);

    TEST_ASSERT_TRUE(out.raised);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence * 0.8f, out.confidence);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::LowSignalQuality), out.reasons);
}

void test_detector_not_worn_resets_history(void) {
    EventCandidateDetector detector;
    const MotionFeatures motion = makeMotion(1000, 0.25f, 30.0f, 0.5f);
    const PpgFeatures ppg = makePpg(1000, 0.0f, 0.0f, false);
    const StateSnapshot worn = makeState(1000);

    stepWindow(detector, 1000, motion, ppg, worn);
    stepWindow(detector, 2000, motion, ppg, worn);
    TEST_ASSERT_EQUAL_UINT32(2, detector.historyDepth());

    const StateSnapshot off = makeState(3000, bw::WearState::NotWorn);
    const CandidateOutput out = stepWindow(detector, 3000, motion, ppg, off);

    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::DeviceNotWorn), out.reasons);
    TEST_ASSERT_EQUAL_UINT32(0, detector.historyDepth());
}

void test_detector_gap_restarts_history(void) {
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::Clean);

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);

    // Two missed windows: the drop can no longer be shown within
    // kAbruptDropWindowMs, so the retained windows must not be compared.
    const TimeMs gap_ms = timeline.current.t_ms + 2 * bw::cataplexy::kFeatureWindowMs;
    MotionFeatures late = timeline.current;
    late.t_ms = gap_ms;
    const CandidateOutput out =
        stepWindow(detector, gap_ms, late, timeline.ppg_current, timeline.state);

    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
    TEST_ASSERT_EQUAL_UINT32(1, detector.historyDepth());
}

void test_detector_gap_at_max_keeps_history(void) {
    // Boundary the other way: a gap of EXACTLY kHistoryMaxGapMs (2000 ms) is
    // one missed window, which still proves the drop inside
    // kAbruptDropWindowMs — the retained windows stay and remain usable.
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::Clean);

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);

    const TimeMs gap_ms = timeline.previous.t_ms + bw::cataplexy::kHistoryMaxGapMs;
    MotionFeatures late = timeline.current;
    late.t_ms = gap_ms;
    PpgFeatures ppg_late = timeline.ppg_current;
    ppg_late.t_ms = gap_ms;
    const CandidateOutput out = stepWindow(detector, gap_ms, late, ppg_late, timeline.state);

    TEST_ASSERT_EQUAL_UINT32(3, detector.historyDepth());  // nothing was dropped
    TEST_ASSERT_TRUE(out.raised);                          // and the history still judged this drop
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBaseCandidateConfidence, out.confidence);
}

void test_detector_backwards_time_restarts_history(void) {
    EventCandidateDetector detector;
    DropTimeline timeline = makeDropTimeline(DropScenario::Clean);

    stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
               timeline.state);
    stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
               timeline.state);

    MotionFeatures stale = timeline.earlier;  // older than the last window seen
    const CandidateOutput out =
        stepWindow(detector, stale.t_ms, stale, timeline.ppg_earlier, timeline.state);

    TEST_ASSERT_FALSE(out.raised);
    TEST_ASSERT_EQUAL_UINT32(1, detector.historyDepth());
}

void test_detector_nan_input_never_raises(void) {
    // candidate_detector.cpp: "Non-numeric input (NaN) fails both comparisons,
    // so garbage can never trigger" — check that claim from both sides: NaN in
    // the active window and NaN in the still window.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const DropTimeline timeline = makeDropTimeline(DropScenario::Clean);

    {  // garbage where the motion should be seen as active
        EventCandidateDetector detector;
        stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
                   timeline.state);
        MotionFeatures corrupt = timeline.previous;
        corrupt.enmo_g = nan;
        stepWindow(detector, timeline.previous.t_ms, corrupt, timeline.ppg_previous,
                   timeline.state);
        const CandidateOutput out = stepWindow(detector, timeline.current.t_ms, timeline.current,
                                               timeline.ppg_current, timeline.state);

        TEST_ASSERT_FALSE(out.disabled);
        TEST_ASSERT_FALSE(out.raised);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
        TEST_ASSERT_BITS_LOW(reasonBit(bw::ReasonCode::AbruptMotionDrop), out.reasons);
    }

    {  // garbage where the arm should be seen as still
        EventCandidateDetector detector;
        stepWindow(detector, timeline.earlier.t_ms, timeline.earlier, timeline.ppg_earlier,
                   timeline.state);
        stepWindow(detector, timeline.previous.t_ms, timeline.previous, timeline.ppg_previous,
                   timeline.state);
        MotionFeatures corrupt = timeline.current;
        corrupt.enmo_g = nan;
        corrupt.gravity_tilt_deg = nan;
        corrupt.jerk_rms_g_per_s = nan;
        const CandidateOutput out = stepWindow(detector, timeline.current.t_ms, corrupt,
                                               timeline.ppg_current, timeline.state);

        TEST_ASSERT_FALSE(out.disabled);
        TEST_ASSERT_FALSE(out.raised);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out.confidence);
        TEST_ASSERT_BITS_LOW(reasonBit(bw::ReasonCode::AbruptMotionDrop), out.reasons);
        TEST_ASSERT_EQUAL_UINT8(0, out.confounders);
    }
}

void test_detector_confidence_never_without_reasons(void) {
    const DropScenario scenarios[] = {DropScenario::Clean, DropScenario::SittingDown,
                                      DropScenario::Fall, DropScenario::BriefBurst,
                                      DropScenario::HrDrop};

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        EventCandidateDetector detector;
        const CandidateOutput out = runDrop(detector, scenarios[i]);
        if (out.confidence > 0.0f) {
            TEST_ASSERT_TRUE_MESSAGE(out.reasons != 0, "confidence reported without reasons");
        }
        if (out.raised) {
            TEST_ASSERT_TRUE(out.confidence >= kMinCandidateConfidence);
            TEST_ASSERT_TRUE(out.reasons != 0);
        }
    }
}

void test_detector_event_span_is_the_drop_window(void) {
    EventCandidateDetector detector;
    const DropTimeline timeline = makeDropTimeline(DropScenario::Clean);
    const CandidateOutput out = runDrop(detector, DropScenario::Clean);

    TEST_ASSERT_EQUAL_UINT64(timeline.previous.t_ms, out.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::cataplexy::kAbruptDropWindowMs),
                             out.duration_ms);
}

void test_detector_size_within_budget(void) {
    // Spec §11 counts the raw window buffer elsewhere; the detector itself must
    // stay a small static object (no heap after startup, 00 §7.2).
    char message[72];
    std::snprintf(message, sizeof(message), "sizeof(EventCandidateDetector) = %u B",
                  static_cast<unsigned>(sizeof(EventCandidateDetector)));
    TEST_MESSAGE(message);
    TEST_ASSERT_TRUE(sizeof(EventCandidateDetector) <= 192);
}
