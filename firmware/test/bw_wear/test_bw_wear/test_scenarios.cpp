// Scenario-fixture tests for bw_wear / F04 (spec §9, §14 DoD: the
// pass/fail table for these scenarios goes into the status log).

#include <unity.h>

#include <bw/context.h>
#include <bw/features.h>
#include <bw/wear/wear_config.h>
#include <bw/wear/wear_detector.h>

#include "fixture_helpers.h"

using bw::MotionFeatures;
using bw::PpgFeatures;
using bw::TimeMs;
using bw::WearEstimate;
using bw::WearState;
using bw::wear::WearDetector;
using bw::wear::WearInput;
namespace fx = f04test;

namespace {

WearEstimate stepAt(WearDetector& detector, TimeMs t_ms, const PpgFeatures& ppg,
                    const MotionFeatures& motion) {
    const WearInput input{t_ms, &ppg, &motion};
    return detector.step(input);
}

}  // namespace

// Attached to the wrist, user at rest: Worn exactly after kWornConfirmMs,
// never NotWorn.
void test_scenario_attached_at_rest(void) {
    WearDetector detector;
    bool seen_worn = false;
    TimeMs worn_at = 0;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAttached(t);
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(detector, t, ppg, motion);
        TEST_ASSERT_TRUE(e.state != WearState::NotWorn);
        if (!seen_worn && e.state == WearState::Worn) {
            seen_worn = true;
            worn_at = t;
        }
    }
    TEST_ASSERT_TRUE(seen_worn);
    TEST_ASSERT_EQUAL_UINT64(5000, worn_at);
}

// Worn and moving (pulse partially corrupted, strong micro-motion): Worn
// after the same confirm dwell, never NotWorn.
void test_scenario_moving(void) {
    WearDetector detector;
    bool seen_worn = false;
    TimeMs worn_at = 0;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgMoving(t);
        const MotionFeatures motion = fx::motionMoving(t);
        const WearEstimate e = stepAt(detector, t, ppg, motion);
        TEST_ASSERT_TRUE(e.state != WearState::NotWorn);
        if (!seen_worn && e.state == WearState::Worn) {
            seen_worn = true;
            worn_at = t;
        }
    }
    TEST_ASSERT_TRUE(seen_worn);
    TEST_ASSERT_EQUAL_UINT64(5000, worn_at);
}

// Left on a desk (freshly placed, so the test does not lean on
// kDeskStillnessMs): never Worn, NotWorn once kNotWornConfirmMs passes.
void test_scenario_resting_on_surface(void) {
    WearDetector detector;
    bool seen_worn = false;
    bool seen_not_worn = false;
    TimeMs not_worn_at = 0;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAmbient(t);
        const MotionFeatures motion = fx::motionDesk(t);
        const WearEstimate e = stepAt(detector, t, ppg, motion);
        if (e.state == WearState::Worn) {
            seen_worn = true;
        }
        if (!seen_not_worn && e.state == WearState::NotWorn) {
            seen_not_worn = true;
            not_worn_at = t;
        }
    }
    TEST_ASSERT_FALSE(seen_worn);
    TEST_ASSERT_TRUE(seen_not_worn);
    TEST_ASSERT_EQUAL_UINT64(10000, not_worn_at);
}

// Noise-like plausible HR/SQI in an Insufficient no-contact window cannot
// keep the device from being classified NotWorn.
void test_scenario_no_contact_noisy_hr_stays_not_worn(void) {
    WearDetector detector;
    bool seen_not_worn = false;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        PpgFeatures ppg = fx::ppgAmbient(t);
        ppg.hr_valid = true;
        ppg.hr_bpm = 72.0f;
        ppg.sqi = 1.0f;
        const MotionFeatures motion = fx::motionDesk(t);
        const WearEstimate estimate = stepAt(detector, t, ppg, motion);
        TEST_ASSERT_TRUE(estimate.state != WearState::Worn);
        if (estimate.state == WearState::NotWorn) {
            seen_not_worn = true;
        }
    }
    TEST_ASSERT_TRUE(seen_not_worn);
}

// Worn for a while, then taken off: Worn holds through the confirm dwell
// (a lifted sensor during removal is not yet proof), NotWorn at exactly
// +kNotWornConfirmMs, and never Uncertain (both sources keep reporting).
void test_scenario_removed(void) {
    WearDetector detector;
    for (TimeMs t = 0; t <= 5000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAttached(t);
        const MotionFeatures motion = fx::motionRest(t);
        (void)stepAt(detector, t, ppg, motion);
    }
    TEST_ASSERT_EQUAL_INT((int)WearState::Worn, (int)detector.state());

    bool seen_not_worn = false;
    TimeMs not_worn_at = 0;
    bool seen_uncertain = false;
    for (TimeMs t = 6000; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgAmbient(t);
        const MotionFeatures motion = fx::motionDesk(t);
        const WearEstimate e = stepAt(detector, t, ppg, motion);
        if (e.state == WearState::Uncertain) {
            seen_uncertain = true;
        }
        if (!seen_not_worn && e.state == WearState::NotWorn) {
            seen_not_worn = true;
            not_worn_at = t;
        }
    }
    TEST_ASSERT_TRUE(seen_not_worn);
    TEST_ASSERT_EQUAL_UINT64(16000, not_worn_at);  // removal at 6000 + 10 s
    TEST_ASSERT_FALSE(seen_uncertain);
}

// Lifted sensor with a little skin contact: ambivalent evidence stays
// Uncertain for two full minutes — no false Worn, no false NotWorn (D2).
void test_scenario_loose_contact(void) {
    WearDetector detector;
    for (TimeMs t = 0; t <= 120000; t += 1000) {
        const PpgFeatures ppg = fx::ppgLoose(t);
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(detector, t, ppg, motion);
        TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);
    }
}

// Carried in a pocket: the PPG sees no contact (F02 zero-fills an empty
// window, so D3 counts the source absent) while the IMU keeps seeing human
// motion. KNOWN LIMITATION (spec §9, accepted residual): motion alone can
// carry the score past kWornScoreEnter, so a pocketed device MAY read as
// Worn — always at confidence <= kImuOnlyMaxConfidence (0.6, D5), which
// downstream consumers must treat as weak wear evidence. With only gentle
// micro-motion the score sits inside the hysteresis band and the default
// state is held, so nothing commits at all.
void test_scenario_in_pocket(void) {
    // Gentle micro-motion, no contact: in-band score -> stays Uncertain.
    WearDetector gentle;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgNoData(t);
        const MotionFeatures motion = fx::motionRest(t);
        const WearEstimate e = stepAt(gentle, t, ppg, motion);
        TEST_ASSERT_EQUAL_INT((int)WearState::Uncertain, (int)e.state);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, e.confidence);
    }

    // Sustained carried motion: Worn after kWornConfirmMs, every Worn
    // reading capped (D5), never NotWorn.
    WearDetector carried;
    bool seen_worn = false;
    TimeMs worn_at = 0;
    for (TimeMs t = 0; t <= 60000; t += 1000) {
        const PpgFeatures ppg = fx::ppgNoData(t);
        const MotionFeatures motion = fx::motionMoving(t);
        const WearEstimate e = stepAt(carried, t, ppg, motion);
        TEST_ASSERT_TRUE(e.state != WearState::NotWorn);
        if (e.state == WearState::Worn) {
            if (!seen_worn) {
                seen_worn = true;
                worn_at = t;
            }
            TEST_ASSERT_TRUE(e.confidence <= bw::wear::kImuOnlyMaxConfidence);
        }
    }
    TEST_ASSERT_TRUE(seen_worn);
    TEST_ASSERT_EQUAL_UINT64(5000, worn_at);
}
