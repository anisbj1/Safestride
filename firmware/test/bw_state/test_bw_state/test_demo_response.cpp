#include <bw/state/state_engine.h>
#include <initializer_list>
#include <unity.h>

#include "state_test_support.h"

void test_demo_accepted_response_recovers_all_probabilistic_rungs_not_gates() {
    for (bool sleep : {false, true}) {
        bw::state::StateEngineConfig cfg;
        cfg.ema_alpha = 1.0f;
        bw::state::StateEngine engine{cfg};
        const bw::TimeMs dwell =
            sleep ? cfg.possible_sleep_min_duration_ms : cfg.high_risk_min_duration_ms;
        for (bw::TimeMs t = 0; t <= dwell; t += 1000) {
            auto in = f07_test::freshInputs(t);
            in.alertness.awake = 0.05f;
            in.alertness.drowsy = sleep ? 0.05f : 0.85f;
            in.alertness.sleep = sleep ? 0.90f : 0.10f;
            (void)engine.step(in);
        }
        TEST_ASSERT_EQUAL_INT(static_cast<int>(sleep ? bw::AlertnessState::PossibleSleep
                                                     : bw::AlertnessState::HighDrowsinessRisk),
                              static_cast<int>(engine.state()));
        auto in = f07_test::freshInputs(dwell + 1000);
        in.wake_check_passed = true;
        const auto out = engine.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::AlertnessState::Awake),
                              static_cast<int>(out.snapshot.alertness));
    }
}
