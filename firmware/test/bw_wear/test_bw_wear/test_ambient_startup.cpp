#include <bw/wear/wear_detector.h>
#include <bw/wear/wear_evidence.h>
#include <unity.h>

void test_ambient_noise_never_supports_wear_and_startup_becomes_notworn() {
    bw::wear::WearDetector detector;
    // Untouched bench capture: IR DC ~450, noise PI ~7%, SQI ~0.7. Even a
    // hypothetical noisy HR must not be evidence when absolute contact fails.
    for (bw::TimeMs time = 0; time <= 15000; time += 1000) {
        bw::PpgFeatures ppg{};
        ppg.t_ms = time;
        ppg.quality = bw::DataQuality::Good;
        ppg.ir_dc_level = 450.0f;
        ppg.perfusion_index = 0.07f;
        ppg.sqi = 0.7f;
        ppg.hr_valid = true;
        ppg.hr_bpm = 80.0f;
        bw::MotionFeatures motion{};
        motion.t_ms = time;
        motion.quality = bw::DataQuality::Good;
        // Fixed accel bias can produce ENMO while sitting on the desk; it
        // must not outweigh the computed no-contact PPG evidence.
        motion.enmo_g = 0.03f;
        const auto evidence = bw::wear::evaluateWearEvidence(&ppg, &motion, time);
        TEST_ASSERT_TRUE(evidence.ppg_available);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, evidence.ppg_score);
        const bw::WearEstimate result = detector.step({time, &ppg, &motion});
        if (time >= bw::wear::kNotWornConfirmMs) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::WearState::NotWorn),
                                  static_cast<int>(result.state));
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::WearState::Uncertain),
                                  static_cast<int>(result.state));
        }
    }
}
