// State-machine tests for bw_wear / F04 (spec §6 D1/D2, §9, §10).

#include <unity.h>

#include <bw/context.h>
#include <bw/wear/wear_config.h>
#include <bw/wear/wear_detector.h>

#include "fixture_helpers.h"

using bw::MotionFeatures;
using bw::PpgFeatures;
using bw::TimeMs;
using bw::WearEstimate;
using bw::WearState;
using bw::wear::WearConfig;
using bw::wear::WearDetector;
using bw::wear::WearInput;
namespace fx = f04test;

namespace {

WearEstimate stepAt(WearDetector& detector, TimeMs t_ms, const PpgFeatures* ppg,
                    const MotionFeatures* motion) {
    const WearInput input{t_ms, ppg, motion};
    return detector.step(input);
}

// Repeats one fixture pair every second over [from_ms, to_ms] inclusive.
template <typename PpgFn, typename MotionFn>
WearState runTimeline(WearDetector& detector, TimeMs from_ms, TimeMs to_ms, PpgFn ppg_fn,
                      MotionFn motion_fn, TimeMs* first_state_ms = nullptr,
                      WearState watch = WearState::Worn) {
    WearState last = detector.state();
    for (TimeMs t = from_ms; t <= to_ms; t += 1000) {
        const PpgFeatures ppg = ppg_fn(t);
        const MotionFeatures motion = motion_fn(t);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        if (first_state_ms != nullptr && e.state == watch && !(last == watch)) {
            *first_state_ms = t;
        }
        last = e.state;
    }
    return last;
}

}  // namespace

void test_detector_defaults_to_uncertain(void) {
    WearDetector detector;
    WearEstimate e = stepAt(detector, 0, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);

    e = stepAt(detector, 1000, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);
}

void test_detector_both_sources_unavailable_immediate_uncertain(void) {
    WearDetector detector;
    // Reach Worn with consistent evidence first.
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Absent sources -> Uncertain on the very next tick (§10), no dwell.
    WearEstimate e = stepAt(detector, 6000, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);

    // Back to Worn, then the §10 wording literally: both sources
    // Insufficient (no DC on the PPG, undefined IMU window) -> immediate.
    runTimeline(detector, 7000, 12000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());
    const PpgFeatures no_data = fx::ppgNoData(13000);
    const MotionFeatures bad_motion = fx::motionInsufficient(13000);
    e = stepAt(detector, 13000, &no_data, &bad_motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);
}

void test_detector_enters_worn_after_confirm_dwell(void) {
    WearDetector detector;
    for (TimeMs t = 0; t < 5000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAttached(t);
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    }
    const PpgFeatures ppg = fx::ppgAttached(5000);
    const MotionFeatures motion = fx::motionRest(5000);
    const WearEstimate e = stepAt(detector, 5000, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
    TEST_ASSERT_TRUE(e.confidence >= 0.7f);
}

void test_detector_custom_config_shorter_dwell(void) {
    WearConfig config;
    config.worn_confirm_ms = 2000;
    WearDetector detector(config);

    WearEstimate e = stepAt(detector, 0, nullptr, nullptr);  // reset bias
    const PpgFeatures ppg0 = fx::ppgAttached(0);
    const MotionFeatures motion0 = fx::motionRest(0);
    e = stepAt(detector, 0, &ppg0, &motion0);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);

    const PpgFeatures ppg1 = fx::ppgAttached(1000);
    const MotionFeatures motion1 = fx::motionRest(1000);
    e = stepAt(detector, 1000, &ppg1, &motion1);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);

    const PpgFeatures ppg2 = fx::ppgAttached(2000);
    const MotionFeatures motion2 = fx::motionRest(2000);
    e = stepAt(detector, 2000, &ppg2, &motion2);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
}

void test_detector_custom_config_stricter_enter(void) {
    WearConfig config;
    config.worn_score_enter = 0.9f;  // attached+rest scores 0.789 < 0.9
    WearDetector detector(config);

    const WearState final_state = runTimeline(detector, 0, 60000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)final_state);
}

void test_detector_holds_worn_through_ppg_dropout(void) {
    WearDetector detector;
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // IMU-only fallback: score 0.496 sits in the hysteresis band, so Worn
    // holds across the whole outage and confidence carries the IMU cap (§9).
    for (TimeMs t = 6000; t <= 35000; t += 1000) {
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(detector, t, nullptr, &motion);
        TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
        TEST_ASSERT_TRUE(e.confidence > 0.0f);
        TEST_ASSERT_TRUE(e.confidence <= 0.6f);
    }

    // Contact returns -> full confidence again on the same tick.
    const PpgFeatures ppg = fx::ppgAttached(36000);
    const MotionFeatures motion = fx::motionRest(36000);
    const WearEstimate e = stepAt(detector, 36000, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
    TEST_ASSERT_TRUE(e.confidence > 0.7f);
}

void test_detector_not_worn_only_after_removal_dwell(void) {
    WearDetector detector;
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Removal evidence from t=6000: NotWorn may only commit 10 s later.
    WearState last = WearState::Worn;
    for (TimeMs t = 6000; t <= 16000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAmbient(t);
        const MotionFeatures motion = fx::motionDesk(t);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        if (t < 16000) {
            TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
        } else {
            TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)e.state);
            TEST_ASSERT_TRUE(e.confidence > 0.8f);  // both sources agree
        }
        last = e.state;
    }
    TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)last);
}

void test_detector_hysteresis_band_holds_worn(void) {
    WearDetector detector;
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Loose contact scores 0.475 — inside the 0.4..0.7 band: the state
    // must hold (no flapping) and the confidence must tell the truth.
    const WearState final_state = runTimeline(detector, 6000, 126000, fx::ppgLoose, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)final_state);
}

void test_detector_oscillating_evidence_resets_dwell(void) {
    WearDetector detector;
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Alternate "off" (NotWorn candidate) with "loose" (hold Worn): every
    // candidate change must restart the dwell, so 60 s of oscillation
    // never commits NotWorn. IMU stays at the worn-rest fixture so only
    // the PPG side crosses the exit threshold.
    for (TimeMs t = 6000; t <= 65000; t += 1000) {
        const bool off_tick = ((t - 6000) / 1000) % 2 == 0;
        const PpgFeatures ppg = off_tick ? fx::ppgAmbient(t) : fx::ppgLoose(t);
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
    }

    // Steady removal evidence now commits exactly one dwell later.
    WearState final_state = WearState::Worn;
    for (TimeMs t = 66000; t <= 76000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAmbient(t);
        const MotionFeatures motion = fx::motionDesk(t);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        final_state = e.state;
        if (t < 76000) {
            TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
        }
    }
    TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)final_state);
}

void test_detector_imu_only_worn_with_capped_confidence(void) {
    WearDetector detector;
    WearEstimate e{};
    for (TimeMs t = 0; t <= 5000; t += 1000) {
        const MotionFeatures motion = fx::motionMoving(t);
        e = stepAt(detector, t, nullptr, &motion);
    }
    // IMU alone can reach Worn (positive micro-motion evidence) but never
    // with high confidence (§9).
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
    TEST_ASSERT_TRUE(e.confidence > 0.0f);
    TEST_ASSERT_TRUE(e.confidence <= 0.6f);
}

void test_detector_imu_only_not_worn_confidence_capped(void) {
    // IMU-only NotWorn (PPG absent): raw support would be 1 - 0.30 = 0.70,
    // but without contact evidence the D5 cap keeps it at
    // kImuOnlyMaxConfidence (§9). PPG-only runs are not capped.
    WearDetector detector;
    WearEstimate e{};
    for (TimeMs t = 0; t <= 12000; t += 1000) {
        const MotionFeatures motion = fx::motionDesk(t);
        e = stepAt(detector, t, nullptr, &motion);
        TEST_ASSERT_TRUE(e.confidence <= bw::wear::kImuOnlyMaxConfidence);
    }
    TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)e.state);
    TEST_ASSERT_TRUE(e.confidence > 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(bw::wear::kImuOnlyMaxConfidence, e.confidence);
}

void test_detector_stale_features_fall_back_to_uncertain(void) {
    WearDetector detector;
    // Live features up to t=5000 -> Worn.
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Source stalls: stamps frozen at 5000. Still inside kFeatureStaleMs
    // (inputs 6000..8000) -> Worn holds; past it -> both sources absent.
    for (TimeMs t = 6000; t <= 9000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAttached(5000);
        const MotionFeatures motion = fx::motionRest(5000);
        const WearEstimate e = stepAt(detector, t, &ppg, &motion);
        if (t < 9000) {
            TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
        } else {
            TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
            TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);
        }
    }
}

void test_detector_backward_time_restarts_dwell(void) {
    WearDetector detector;
    runTimeline(detector, 0, 5000, fx::ppgAttached, fx::motionRest);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    // Start a NotWorn dwell, then time jumps backwards (reboot/wrap): the
    // pending dwell must restart, not commit via unsigned underflow.
    PpgFeatures ppg = fx::ppgAmbient(6000);
    MotionFeatures motion = fx::motionDesk(6000);
    WearEstimate e = stepAt(detector, 6000, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);

    ppg = fx::ppgAmbient(1000);
    motion = fx::motionDesk(1000);
    e = stepAt(detector, 1000, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);  // no instant commit

    // Dwell restarts at t=1000: still Worn at 10000 (9 s), commits at 11000.
    for (TimeMs t = 2000; t <= 10000; t += 1000) {
        const PpgFeatures p = fx::ppgAmbient(t);
        const MotionFeatures m = fx::motionDesk(t);
        e = stepAt(detector, t, &p, &m);
        TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)e.state);
    }
    ppg = fx::ppgAmbient(11000);
    motion = fx::motionDesk(11000);
    e = stepAt(detector, 11000, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)e.state);
}

void test_detector_never_worn_without_positive_evidence(void) {
    // PPG ambient leak + desk motion for a minute: never Worn (§5).
    WearDetector detector;
    WearEstimate e{};
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAmbient(t);
        const MotionFeatures motion = fx::motionDesk(t);
        e = stepAt(detector, t, &ppg, &motion);
        TEST_ASSERT_TRUE(e.state != WearState::Worn);
    }
    TEST_ASSERT_EQUAL_INT((int)WearState::NotWorn, (int)e.state);

    // Same with the PPG stalled (IMU-only 0.30 <= exit): still no Worn.
    WearDetector detector2;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgNoData(t);
        const MotionFeatures motion = fx::motionDesk(t);
        e = stepAt(detector2, t, &ppg, &motion);
        TEST_ASSERT_TRUE(e.state != WearState::Worn);
    }
}

void test_detector_size_within_budget(void) {
    // Spec §11: RAM < 0.5 KB for the module's persistent state.
    TEST_ASSERT_TRUE(sizeof(WearDetector) < 512);
    TEST_ASSERT_TRUE(sizeof(WearInput) <= 32);
}

void test_detector_estimate_echoes_tick_time(void) {
    // The estimate carries the tick stamp (not a feature stamp), so a
    // consumer can attribute it to the window it just stepped over.
    WearDetector detector;
    const PpgFeatures ppg = fx::ppgAttached(1234);
    const MotionFeatures motion = fx::motionRest(1234);

    WearEstimate e = stepAt(detector, 1234, nullptr, nullptr);
    TEST_ASSERT_EQUAL_UINT64(1234, e.t_ms);

    e = stepAt(detector, 1234, &ppg, &motion);
    TEST_ASSERT_EQUAL_UINT64(1234, e.t_ms);

    // Stale pair (both sources absent, §10): the stamp still echoes.
    e = stepAt(detector, 5678, &ppg, &motion);
    TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
    TEST_ASSERT_EQUAL_UINT64(5678, e.t_ms);
}
