// F08 §9 test_classifier.cpp — the §6 D2 rule table row by row, both clock
// states, and the §8 boundaries (kNapMaxMs −1/=/+1 ms; kMaxDegradedShare at
// exactly 0.5 vs 0.5 + ε).

#include <bw/sleep/episode_classifier.h>
#include <bw/sleep/sleep_config.h>
#include <unity.h>

#include "sleep_test_support.h"

using bw::EpisodeClass;
using bw::OperatingMode;
using bw::UnixSeconds;
using bw::sleep::ClassifyInput;
using bw::sleep::EpisodeClassifier;
using bw::sleep::SleepEpisodeConfig;

namespace {

constexpr SleepEpisodeConfig kCfg{};
// Wall time known: any non-zero value (DEC-009: 0 is "unknown", never a date).
constexpr UnixSeconds kSynced = 1'700'000'000;
constexpr bw::TimeMs kFortyMin = 40u * 60u * 1000u;

}  // namespace

// ---- row 1: degraded share beats duration ----------------------------------

void test_classifier_row1_degraded_share_above_max_uncertain(void) {
    // DoD §14 example: 50 min span, PPG dead for 62 % of it → Uncertain.
    const ClassifyInput in{50u * 60u * 1000u, 0.62f, OperatingMode::Day, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Uncertain),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row1_share_exactly_max_passes(void) {
    // §8 boundary: the comparison is `>`, so exactly kMaxDegradedShare falls
    // through to the duration/mode rows — 40 min Day + synced → Rest.
    const ClassifyInput in{kFortyMin, kCfg.kMaxDegradedShare, OperatingMode::Day, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Rest),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row1_share_epsilon_above_max_uncertain(void) {
    const ClassifyInput in{kFortyMin, kCfg.kMaxDegradedShare + 1e-6f, OperatingMode::Day, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Uncertain),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

// ---- row 2: duration beats mode --------------------------------------------

void test_classifier_row2_duration_at_napmax_is_main_sleep(void) {
    // §8 boundary: duration is inclusive at kNapMaxMs; a 3 h planned nap is
    // recorded as MainSleep (intent loses to duration, deliberately).
    const ClassifyInput in{kCfg.kNapMaxMs, 0.0f, OperatingMode::Nap, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::MainSleep),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row2_napmax_minus_one_is_not_main_sleep(void) {
    const ClassifyInput in{kCfg.kNapMaxMs - 1, 0.0f, OperatingMode::Nap, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Nap),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row2_napmax_plus_one_main_sleep_day(void) {
    // Duration beats even "no clock" (row 2 before row 4).
    const ClassifyInput in{kCfg.kNapMaxMs + 1, 0.0f, OperatingMode::Day, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::MainSleep),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

// ---- row 3: planned nap needs no clock --------------------------------------

void test_classifier_row3_mode_nap_with_unsynced_clock(void) {
    // DoD §14 example: 35 min after StartNap, clock unknown → Nap.
    const ClassifyInput in{35u * 60u * 1000u, 0.0f, OperatingMode::Nap, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Nap),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

// ---- row 4: unknown wall time never claims day/night -------------------------

void test_classifier_row4_unsynced_clock_day_is_uncertain(void) {
    // DoD §14 example: 70 min in mode Day, no sync yet → Uncertain.
    const ClassifyInput in{70u * 60u * 1000u, 0.0f, OperatingMode::Day, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Uncertain),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

// ---- row 5: night mode --------------------------------------------------------

void test_classifier_row5_night_short_of_napmax_known_clock_is_nap(void) {
    // 1 h 20 min night fragment, synced clock → Nap (short of kNapMaxMs).
    const ClassifyInput in{80u * 60u * 1000u, 0.0f, OperatingMode::Night, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Nap),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row5_night_unsynced_clock_is_uncertain(void) {
    // Ordering is deliberate: row 4 (no clock) fires before row 5 (mode).
    const ClassifyInput in{80u * 60u * 1000u, 0.0f, OperatingMode::Night, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Uncertain),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

// ---- row 6: synced day-ish modes are rests -----------------------------------

void test_classifier_row6_day_synced_is_rest(void) {
    // DoD §14 example: 40 min afternoon lie-down, synced → Rest.
    const ClassifyInput in{kFortyMin, 0.0f, OperatingMode::Day, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Rest),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_row6_calibration_and_research_synced_are_rest(void) {
    const ClassifyInput calibration{kFortyMin, 0.0f, OperatingMode::Calibration, kSynced};
    const ClassifyInput research{kFortyMin, 0.0f, OperatingMode::Research, kSynced};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Rest),
                          static_cast<int>(EpisodeClassifier::classify(calibration)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Rest),
                          static_cast<int>(EpisodeClassifier::classify(research)));
}

// ---- DoD §14 worked examples --------------------------------------------------

void test_classifier_duration_beats_mode_planned_nap(void) {
    // 4 h 10 min (= 250 min) planned nap, no clock → MainSleep (row 2, before rows 3–5).
    const ClassifyInput in{250u * 60u * 1000u, 0.0f, OperatingMode::Nap, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::MainSleep),
                          static_cast<int>(EpisodeClassifier::classify(in)));
}

void test_classifier_varied_config_moves_boundary(void) {
    // The config is passed in, so a host test varies one value without
    // touching globals (§7): with a 1 h nap cap, 70 min becomes MainSleep.
    SleepEpisodeConfig cfg{};
    cfg.kNapMaxMs = 60u * 60u * 1000u;
    const ClassifyInput in{70u * 60u * 1000u, 0.0f, OperatingMode::Day, 0};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::MainSleep),
                          static_cast<int>(EpisodeClassifier::classify(in, cfg)));
}
