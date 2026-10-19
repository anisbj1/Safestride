// bw_diag — PpgQualityAssessor tests (spec §15.5 item 3; D2/D6 rules and
// the PpgFlatline / PpgSaturated / PpgPoorContact cause/remedy rows of
// §15.3).

#include <bw/diag/diag_config.h>
#include <bw/diag/ppg_quality_assessor.h>
#include <unity.h>

#include "diag_test_support.h"

using bw::DataQuality;
using bw::PpgSample;
using bw::SensorStatus;
using bw::TimeMs;
using bw::diag::PpgQualityAssessor;
using bw_diag_test::flatlinePpgWindow;
using bw_diag_test::healthyPpg;
using bw_diag_test::healthyPpgWindow;
using bw_diag_test::saturatedPpgWindow;

namespace {

constexpr auto kGood = DataQuality::Good;
constexpr auto kDegraded = DataQuality::Degraded;
constexpr auto kInsufficient = DataQuality::Insufficient;

}  // namespace

// Healthy tick: Ok status + fresh, pulsating frame ⇒ Good, no edge.
void test_ppg_healthy_is_good(void) {
    PpgQualityAssessor a;
    const bw::PpgFeatures f = healthyPpg(1000);
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Bench case (§13): PPG absent — Failed status, no features — ⇒
// Insufficient quality only; F01 reports PpgAbsent/PpgReadFailed, F23
// never double-reports (D6).
void test_ppg_bench_absent_no_f23_edge(void) {
    PpgQualityAssessor a;
    auto out = a.step(1000, true, SensorStatus::Failed, nullptr, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));

    out = a.step(2000, false, SensorStatus::Uninitialized, nullptr, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Fail-safe (§10, §15.5 item 3): status Ok but no feature frames yet ⇒
// Insufficient, never Good by absence.
void test_ppg_status_ok_no_features_insufficient(void) {
    PpgQualityAssessor a;
    const auto out = a.step(1000, true, SensorStatus::Ok, nullptr, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// §15.3 row PpgPoorContact: PI 0.001 < kPoorContactPerfusionIndex while
// the stream is present ⇒ Warning + Degraded. Remedy: tighten band /
// re-seat; user re-wears (F04 context).
void test_ppg_poor_contact_pi_0_001_warns(void) {
    PpgQualityAssessor a;
    const bw::PpgFeatures f = healthyPpg(1000, 0.001f);
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgPoorContact),
                          static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Warning),
                          static_cast<int>(out.edge.severity));
}

// PI 0.003 clears the threshold: no poor-contact fault.
void test_ppg_poor_contact_none_at_pi_0_003(void) {
    PpgQualityAssessor a;
    const bw::PpgFeatures f = healthyPpg(1000, 0.003f);
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// §15.3 row PpgFlatline (feature path): kStuckFeatureWindows identical
// ir_dc_level frames with PI≈0 ⇒ Error + Insufficient. Remedy: clean and
// reseat the sensor; F26 reinit.
void test_ppg_flatline_identical_dc_x5_low_pi(void) {
    PpgQualityAssessor a;
    bw::diag::PpgAssessOutput out{};
    for (TimeMs t = 1000; t < 6000; t += 1000) {
        const bw::PpgFeatures f = healthyPpg(t, 0.0005f);  // constant DC, no pulsation
        out = a.step(t, true, SensorStatus::Ok, &f, nullptr, 0);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgFlatline),
                          static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(out.edge.severity));
}

// A constant DC with HEALTHY perfusion is a legitimate constant offset,
// not a dead sensor — flat-line needs both conditions.
void test_ppg_constant_dc_with_perfusion_is_not_flatline(void) {
    PpgQualityAssessor a;
    bw::diag::PpgAssessOutput out{};
    for (TimeMs t = 1000; t < 6000; t += 1000) {
        const bw::PpgFeatures f = healthyPpg(t, 0.004f);  // constant DC, strong pulse
        out = a.step(t, true, SensorStatus::Ok, &f, nullptr, 0);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));
}

// §15.3 row PpgSaturated (raw path): saturated fraction >= 2 % ⇒ Error +
// Insufficient; 1.9 % ⇒ none. (Feed is host-only at M2, D6; M5.)
void test_ppg_raw_saturated_2pct_faults_1_9pct_does_not(void) {
    PpgQualityAssessor a;
    PpgSample window[100];

    saturatedPpgWindow(window, 100, 2);  // 2 % exactly
    auto eval = a.evaluatePpgRawWindow(window, 100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgSaturated),
                          static_cast<int>(eval.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(eval.instantaneous));

    saturatedPpgWindow(window, 100, 1);  // 1 % — below the threshold
    eval = a.evaluatePpgRawWindow(window, 100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(eval.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(eval.instantaneous));
}

// §15.3 row PpgFlatline (raw path): 250 identical IR samples ⇒ Error;
// 249 ⇒ below kPpgFlatlineRawSamples, none.
void test_ppg_raw_flatline_250_faults_249_does_not(void) {
    PpgQualityAssessor a;
    PpgSample window[250];

    flatlinePpgWindow(window, 250);
    auto eval = a.evaluatePpgRawWindow(window, 250);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgFlatline),
                          static_cast<int>(eval.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(eval.instantaneous));

    flatlinePpgWindow(window, 249);
    eval = a.evaluatePpgRawWindow(window, 249);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(eval.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(eval.instantaneous));
}

// D6 honesty: an empty/absent raw window is "not evaluated" — Insufficient
// answer from the evaluator, never a fabricated Good.
void test_ppg_raw_empty_window_not_evaluated(void) {
    PpgQualityAssessor a;
    PpgSample one{};
    auto eval = a.evaluatePpgRawWindow(nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(eval.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(eval.edge.code));

    eval = a.evaluatePpgRawWindow(&one, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(eval.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(eval.edge.code));
}

// Audit L4 (§15.4 raw-window rules): stale features + a raw window. The
// raw-Good override reports the raw evaluation itself (Good possible),
// but a defective window forces Insufficient + its edge — and the
// Degraded driver hint stays a floor (checked in the worst-of tests).
void test_ppg_stale_features_raw_overrides_staleness(void) {
    PpgQualityAssessor a;
    PpgSample window[100];
    const bw::PpgFeatures stale = healthyPpg(1000 - bw::diag::kFeatureStalenessMs - 1);

    // Saturated window + stale features: the raw evidence beats the
    // staleness fail-safe — Insufficient + PpgSaturated.
    saturatedPpgWindow(window, 100, 2);
    auto out = a.step(1000, true, SensorStatus::Ok, &stale, window, 100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgSaturated),
                          static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));

    // Healthy window + stale features: the raw-Good override — the raw
    // evaluation is current evidence and reports Good, never fabricated.
    healthyPpgWindow(window, 100);
    out = a.step(2000, true, SensorStatus::Ok, &stale, window, 100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(out.instantaneous));

    // No window + stale features: back to the fail-safe answer.
    out = a.step(3000, true, SensorStatus::Ok, &stale, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(out.instantaneous));
}

// F02's own quality hint: worst-of, never raises above the rules.
void test_ppg_f02_hint_worst_of(void) {
    PpgQualityAssessor a;
    bw::PpgFeatures f = healthyPpg(1000);
    f.quality = kDegraded;
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
}

// Cross-modal hint: motion_corrupted ⇒ at least Degraded even with good
// perfusion (§15.5 item 3).
void test_ppg_motion_corrupted_at_least_degraded(void) {
    PpgQualityAssessor a;
    bw::PpgFeatures f = healthyPpg(1000);
    f.motion_corrupted = true;
    const auto out = a.step(1000, true, SensorStatus::Ok, &f, nullptr, 0);
    TEST_ASSERT_TRUE(static_cast<int>(out.instantaneous) >= static_cast<int>(kDegraded));
}

// Worst-of with the F01 driver hint (§15.4): a Degraded sensor status
// degrades the source even when every feature-level rule is healthy —
// the same mapping the IMU assessor applies (audit L2 alignment).
void test_ppg_degraded_status_worst_of(void) {
    PpgQualityAssessor a;
    const bw::PpgFeatures f = healthyPpg(1000);
    const auto out = a.step(1000, true, SensorStatus::Degraded, &f, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// The driver hint is also a floor on the raw-override path (§15.4): a
// healthy raw window on a stale-feature tick reports the raw evaluation
// worst-of the Degraded status — never Good by the raw window alone.
void test_ppg_degraded_status_stale_features_raw_worst_of(void) {
    PpgQualityAssessor a;
    PpgSample window[50];
    healthyPpgWindow(window, 50);
    const bw::PpgFeatures stale = healthyPpg(1000 - bw::diag::kFeatureStalenessMs - 1);
    const auto out = a.step(1000, true, SensorStatus::Degraded, &stale, window, 50);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(out.instantaneous));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out.edge.code));
}

// Edge-once-per-episode: persistent poor contact reports once; clearing
// and recurring edges again (rate limiting is Diagnostics' D9 job).
void test_ppg_poor_contact_reedges_after_clear(void) {
    PpgQualityAssessor a;
    const bw::PpgFeatures low1 = bw_diag_test::healthyPpg(1000, 0.001f);
    const auto out1 = a.step(1000, true, SensorStatus::Ok, &low1, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgPoorContact),
                          static_cast<int>(out1.edge.code));

    const bw::PpgFeatures good = bw_diag_test::healthyPpg(2000, 0.004f);
    const auto out2 = a.step(2000, true, SensorStatus::Ok, &good, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None), static_cast<int>(out2.edge.code));

    const bw::PpgFeatures low2 = bw_diag_test::healthyPpg(3000, 0.001f);
    const auto out3 = a.step(3000, true, SensorStatus::Ok, &low2, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::PpgPoorContact),
                          static_cast<int>(out3.edge.code));
}
