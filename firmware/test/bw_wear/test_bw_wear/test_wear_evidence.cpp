// Evidence-scoring tests for bw_wear / F04 (spec §7 WearEvidence, D3/D4).

#include <cmath>

#include <unity.h>

#include <bw/wear/wear_config.h>
#include <bw/wear/wear_evidence.h>

#include "fixture_helpers.h"

using bw::MotionFeatures;
using bw::PpgFeatures;
using bw::TimeMs;
using bw::wear::evaluateWearEvidence;
using bw::wear::kDeskStillnessMs;
using bw::wear::kFeatureStaleMs;
using bw::wear::kPpgContactIrDcFull;
using bw::wear::kPpgContactIrDcMin;
using bw::wear::kPpgPerfusionFull;
using bw::wear::kPpgPerfusionMin;
using bw::wear::WearEvidence;
namespace fx = f04test;

void test_evidence_fresh_sources_scored(void) {
    const TimeMs now = 10000;
    const PpgFeatures ppg = fx::ppgAttached(now);
    const MotionFeatures motion = fx::motionRest(now);

    const WearEvidence ev = evaluateWearEvidence(&ppg, &motion, now);

    TEST_ASSERT_TRUE(ev.ppg_available);
    TEST_ASSERT_TRUE(ev.imu_available);
    // contact 1, perfusion 1, hr 1, sqi 0.9 -> 0.4+0.25+0.2+0.135
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.985f, ev.ppg_score);
    // micro 0.28 above desk floor, stillness young -> 0.196+0.30
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.496f, ev.imu_score);
}

void test_evidence_null_sources_absent(void) {
    const WearEvidence ev = evaluateWearEvidence(nullptr, nullptr, 5000);
    TEST_ASSERT_FALSE(ev.ppg_available);
    TEST_ASSERT_FALSE(ev.imu_available);
}

void test_evidence_stale_feature_absent(void) {
    const TimeMs now = 10000;
    const PpgFeatures ppg = fx::ppgAttached(now - kFeatureStaleMs - 1);
    const MotionFeatures motion = fx::motionRest(now - kFeatureStaleMs);

    const WearEvidence ev = evaluateWearEvidence(&ppg, &motion, now);

    TEST_ASSERT_FALSE(ev.ppg_available);  // one ms past the stale window
    TEST_ASSERT_TRUE(ev.imu_available);   // exactly at the stale window
}

void test_evidence_future_stamp_absent(void) {
    const TimeMs now = 1000;
    const PpgFeatures ppg = fx::ppgAttached(now + 500);  // clock pairing is off
    const WearEvidence ev = evaluateWearEvidence(&ppg, nullptr, now);
    TEST_ASSERT_FALSE(ev.ppg_available);
}

void test_evidence_zero_dc_ppg_absent(void) {
    // Fresh stamp but no computed DC (F02 zero-fills an empty window): the
    // source produced nothing, it is not "evidence of no contact".
    const TimeMs now = 20000;
    const PpgFeatures ppg = fx::ppgNoData(now);
    const WearEvidence ev = evaluateWearEvidence(&ppg, nullptr, now);
    TEST_ASSERT_FALSE(ev.ppg_available);
}

void test_evidence_insufficient_rules(void) {
    const TimeMs now = 30000;
    // IMU Insufficient = undefined window statistics -> absent.
    const MotionFeatures bad_motion = fx::motionInsufficient(now);
    WearEvidence ev = evaluateWearEvidence(nullptr, &bad_motion, now);
    TEST_ASSERT_FALSE(ev.imu_available);

    // PPG Insufficient-perfusion WITH a computed DC -> present (D3): the
    // low pulsatility must score low, not vanish; Insufficient quality gates
    // the SQI term while retaining computed DC and perfusion.
    const PpgFeatures loose = fx::ppgLoose(now);
    ev = evaluateWearEvidence(&loose, nullptr, now);
    TEST_ASSERT_TRUE(ev.ppg_available);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.43125f, ev.ppg_score);
    TEST_ASSERT_TRUE(ev.ppg_score > 0.4f && ev.ppg_score < 0.7f);
}

void test_evidence_insufficient_gates_hr_and_sqi_but_keeps_contact(void) {
    const TimeMs now = 31000;
    PpgFeatures ppg = fx::ppgAttached(now);
    ppg.quality = bw::DataQuality::Insufficient;
    const WearEvidence insufficient = evaluateWearEvidence(&ppg, nullptr, now);
    TEST_ASSERT_TRUE(insufficient.ppg_available);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.65f, insufficient.ppg_score);

    ppg.quality = bw::DataQuality::Good;
    const WearEvidence good = evaluateWearEvidence(&ppg, nullptr, now);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.985f, good.ppg_score);

    // Low perfusion still retains DC/contact score, independently of HR/SQI.
    ppg.quality = bw::DataQuality::Insufficient;
    ppg.perfusion_index = 0.0015f;
    ppg.sqi = 0.0f;
    ppg.hr_valid = false;
    const WearEvidence low_perfusion = evaluateWearEvidence(&ppg, nullptr, now);
    TEST_ASSERT_TRUE(low_perfusion.ppg_available);
    TEST_ASSERT_TRUE(low_perfusion.ppg_score > 0.4f);
}

void test_evidence_score_boundaries(void) {
    const TimeMs now = 40000;

    // Exactly at the minimum thresholds -> 0, not epsilon.
    PpgFeatures floor_ppg{};
    floor_ppg.t_ms = now;
    floor_ppg.quality = bw::DataQuality::Insufficient;
    floor_ppg.ir_dc_level = kPpgContactIrDcMin;
    floor_ppg.perfusion_index = kPpgPerfusionMin;
    WearEvidence ev = evaluateWearEvidence(&floor_ppg, nullptr, now);
    TEST_ASSERT_TRUE(ev.ppg_available);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.ppg_score);

    // At/above the full thresholds with HR and SQI -> 1 (within rounding).
    PpgFeatures full_ppg{};
    full_ppg.t_ms = now;
    full_ppg.quality = bw::DataQuality::Good;
    full_ppg.ir_dc_level = kPpgContactIrDcFull;
    full_ppg.perfusion_index = kPpgPerfusionFull;
    full_ppg.hr_valid = true;
    full_ppg.sqi = 1.0f;
    ev = evaluateWearEvidence(&full_ppg, nullptr, now);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, ev.ppg_score);

    // IMU at the desk floor -> micro sub-score 0, young stillness -> 0.30.
    const MotionFeatures desk = fx::motionDesk(now);
    ev = evaluateWearEvidence(nullptr, &desk, now);
    TEST_ASSERT_TRUE(ev.imu_available);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, ev.imu_score);

    // Strong motion -> micro 1 and stillness young -> full score.
    const MotionFeatures moving = fx::motionMoving(now);
    ev = evaluateWearEvidence(nullptr, &moving, now);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, ev.imu_score);
}

void test_evidence_desk_long_still_zero(void) {
    const TimeMs now = 50000;
    const MotionFeatures desk = fx::motionDeskLongStill(now);
    const WearEvidence ev = evaluateWearEvidence(nullptr, &desk, now);
    TEST_ASSERT_TRUE(ev.imu_available);
    // Below the desk floor AND untouched past kDeskStillnessMs -> no
    // positive evidence at all (the removed-from-wrist IMU signature).
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.imu_score);
    TEST_ASSERT_TRUE(kDeskStillnessMs > 0);
}

void test_evidence_nonfinite_inputs_safe(void) {
    const TimeMs now = 60000;

    PpgFeatures nan_ppg = fx::ppgAttached(now);
    nan_ppg.ir_dc_level = std::nanf("");
    WearEvidence ev = evaluateWearEvidence(&nan_ppg, nullptr, now);
    TEST_ASSERT_FALSE(ev.ppg_available);  // NaN fails `> 0` -> absent

    MotionFeatures nan_motion = fx::motionRest(now);
    nan_motion.enmo_g = std::nanf("");
    ev = evaluateWearEvidence(nullptr, &nan_motion, now);
    TEST_ASSERT_TRUE(ev.imu_available);
    TEST_ASSERT_FALSE(std::isnan(ev.imu_score));  // clamped, never NaN
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, ev.imu_score);
}
