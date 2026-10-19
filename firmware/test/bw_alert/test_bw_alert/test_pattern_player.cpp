// bw_alert - PatternPlayer tests (F10 §9: "pattern player timing accuracy
// against RecordingLed/Buzzer"). The player is driven by an explicit `now`,
// so every transition time is asserted rather than slept for; the tolerance is
// one AlertTask tick (kAlertTickPeriodMs), which is the real quantisation the
// device has (F10 §11: 20 Hz).
//
// The FakeClock is advanced alongside every tick: RecordingLed/RecordingBuzzer
// stamp their entries from it (bw_testkit), so a test that forgot to advance
// it would record t_ms = 0 for every transition.

#include <unity.h>

#include <bw/alert/alert_patterns.h>
#include <bw/alert/pattern_player.h>
#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_buzzer.h>
#include <bw/testkit/recording_led.h>

namespace ba = bw::alert;
using bw::Rgb;
using bw::TimeMs;

namespace {

namespace tk = bw::testkit;

// One AlertTask tick: advance the clock, then step the player.
void tickAt(tk::FakeClock& clock, ba::PatternPlayer& player, TimeMs t) {
    clock.setMs(t);
    player.tick(t);
}

// Runs the player over [0, at] at exactly the AlertTask cadence.
void run(tk::FakeClock& clock, ba::PatternPlayer& player, TimeMs at) {
    for (TimeMs t = 0; t <= at; t += ba::kAlertTickPeriodMs) {
        tickAt(clock, player, t);
    }
}

void assertRgb(Rgb expected, Rgb actual, const char* msg) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.r, actual.r, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.g, actual.g, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.b, actual.b, msg);
}

}  // namespace

// --- ladder ⇒ pattern mapping (spec §6 D3, mirrored by the diagram) ---------

void test_patterns_map_levels(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Off),
                          static_cast<int>(ba::patternForLevel(bw::AlertLevel::Normal)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Warning),
                          static_cast<int>(ba::patternForLevel(bw::AlertLevel::Warning)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Active),
                          static_cast<int>(ba::patternForLevel(bw::AlertLevel::Active)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Persistent),
                          static_cast<int>(ba::patternForLevel(bw::AlertLevel::Persistent)));
}

// --- first call / empty state (host-testing item 2) -------------------------

void test_player_off_is_dark_and_silent(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);

    player.select(ba::PatternId::Off);  // already Off: no restart
    tickAt(clock, player, 0);

    TEST_ASSERT_EQUAL_UINT(1, led.count());
    TEST_ASSERT_EQUAL_UINT(1, buzzer.count());
    assertRgb(ba::kLedOff, led.last().rgb, "Off must write a dark LED once");
    TEST_ASSERT_FALSE(buzzer.last().on);

    run(clock, player, 5000);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, led.count(), "the Off pattern must not churn writes");
    TEST_ASSERT_EQUAL_UINT(1, buzzer.count());
}

// --- timing ------------------------------------------------------------------

void test_player_warning_two_beeps_once_then_silent_flashes(void) {
    TEST_ASSERT_EQUAL_UINT32(10000, ba::kWarningToActiveMs);
    TEST_ASSERT_EQUAL_UINT32(1000, ba::kDeescalateHoldMs);
    TEST_ASSERT_EQUAL_UINT32(8000, ba::kStateStaleMs);
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);
    player.select(ba::PatternId::Warning);

    run(clock, player, ba::kWarningToActiveMs - 1);

    TEST_ASSERT_TRUE(led.count() > 4);
    assertRgb(ba::kLedWarning, led.at(0).rgb, "warning is amber");
    assertRgb(ba::kLedOff, led.at(1).rgb, "then dark");
    TEST_ASSERT_TRUE(buzzer.at(0).on);
    TEST_ASSERT_FALSE(buzzer.at(1).on);
    TEST_ASSERT_TRUE(buzzer.at(2).on);
    TEST_ASSERT_FALSE(buzzer.at(3).on);
    for (unsigned i = 4; i < buzzer.count(); ++i) {
        TEST_ASSERT_FALSE_MESSAGE(buzzer.at(i).on, "warning tail stays silent");
    }
    TEST_ASSERT_TRUE(ba::kWarningSteps[0].duration_ms + ba::kWarningSteps[1].duration_ms +
                         ba::kWarningSteps[2].duration_ms + ba::kWarningSteps[3].duration_ms +
                         20 * ba::kBlinkHalfMs >
                     ba::kWarningToActiveMs);
}

void test_player_active_burst_then_rest(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);
    player.select(ba::PatternId::Active);

    tickAt(clock, player, 0);
    TEST_ASSERT_TRUE(buzzer.last().on);
    TEST_ASSERT_EQUAL_UINT16(ba::kBuzzerFreqHz, buzzer.last().freq_hz);
    assertRgb(ba::kLedActive, led.last().rgb, "Active lights the LED with the beep");

    tickAt(clock, player, ba::kActiveToneMs);
    TEST_ASSERT_FALSE_MESSAGE(buzzer.last().on, "the tone ends after kBeepToneMs");

    tickAt(clock, player, ba::kActiveToneMs + ba::kBeepGapMs);
    TEST_ASSERT_TRUE_MESSAGE(buzzer.last().on, "the next beep follows the gap");

    // Third beep ends at 950 ms, then kBeepRestMs of silence (§6 D3).
    const TimeMs third_beep_end = 3 * ba::kActiveToneMs + 2 * ba::kBeepGapMs;
    const TimeMs cycle = third_beep_end + ba::kBeepRestMs;
    tickAt(clock, player, third_beep_end);
    TEST_ASSERT_FALSE(buzzer.last().on);

    const unsigned entries_before = buzzer.count();
    tickAt(clock, player, cycle - 1);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(entries_before, buzzer.count(),
                                   "the rest is really silent, not a new write");
    tickAt(clock, player, cycle);
    TEST_ASSERT_TRUE_MESSAGE(buzzer.last().on, "the burst repeats after kBeepRestMs");
}

void test_player_persistent_is_more_insistent_than_active(void) {
    // §6 D3: Persistent must be clearly worse than Active. With no volume
    // control (02 §3) the duty cycle is the only lever: 500/800 vs 450/3950.
    tk::FakeClock clock;
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(nullptr, &buzzer);
    player.select(ba::PatternId::Persistent);

    tickAt(clock, player, 0);
    TEST_ASSERT_TRUE(buzzer.last().on);
    tickAt(clock, player, ba::kToneOnMs);
    TEST_ASSERT_FALSE(buzzer.last().on);
    tickAt(clock, player, ba::kToneOnMs + ba::kToneGapMs);
    TEST_ASSERT_TRUE_MESSAGE(buzzer.last().on, "Persistent comes back after a short gap only");

    const float persistent_duty =
        static_cast<float>(ba::kToneOnMs) / static_cast<float>(ba::kToneOnMs + ba::kToneGapMs);
    const float active_duty =
        static_cast<float>(3 * ba::kActiveToneMs) /
        static_cast<float>(3 * ba::kActiveToneMs + 2 * ba::kBeepGapMs + ba::kBeepRestMs);
    TEST_ASSERT_TRUE_MESSAGE(persistent_duty > active_duty,
                             "Persistent must outrun Active in duty cycle");
}

// --- idempotence / robustness -----------------------------------------------

void test_player_reselect_does_not_restart(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);

    player.select(ba::PatternId::Warning);
    tickAt(clock, player, 0);
    TEST_ASSERT_EQUAL_UINT(1, led.count());

    tickAt(clock, player, 250);
    player.select(ba::PatternId::Warning);  // same id: no-op
    tickAt(clock, player, 260);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, led.count(),
                                   "re-selecting the running pattern must not restart it");
}

void test_player_intensity_scales_the_led(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);

    player.setIntensity(50, true);
    player.select(ba::PatternId::Warning);
    tickAt(clock, player, 0);

    const Rgb expected{static_cast<uint8_t>(255 * 50 / 100), static_cast<uint8_t>(128 * 50 / 100),
                       0};
    assertRgb(expected, led.last().rgb, "50 % gain halves every channel");
    assertRgb(expected, player.appliedLed(), "appliedLed mirrors the gain");
}

void test_player_buzzer_disabled_by_profile(void) {
    tk::FakeClock clock;
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(nullptr, &buzzer);

    player.setIntensity(100, false);  // e.g. a mode profile with buzzer off
    player.select(ba::PatternId::Active);
    run(clock, player, 2000);

    TEST_ASSERT_TRUE_MESSAGE(buzzer.count() >= 1, "the profile is still written");
    for (unsigned i = 0; i < buzzer.count(); ++i) {
        TEST_ASSERT_FALSE_MESSAGE(buzzer.at(i).on, "buzzer_enabled=false must be silent");
    }
}

void test_player_null_actuators_are_safe(void) {
    ba::PatternPlayer player;  // nothing attached yet (deferred DI)
    player.select(ba::PatternId::Persistent);
    player.tick(0);
    player.tick(60000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Persistent),
                          static_cast<int>(player.current()));
}

void test_player_time_jump_lands_on_one_step_without_spam(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);
    player.select(ba::PatternId::Active);

    tickAt(clock, player, 0);
    TEST_ASSERT_EQUAL_UINT(1, led.count());

    tickAt(clock, player, 1000000);  // a stalled task wakes up ~17 min late
    TEST_ASSERT_TRUE_MESSAGE(led.count() <= 2,
                             "a big jump applies at most one new output, never one write per tick");
    TEST_ASSERT_EQUAL_UINT(0, led.dropped());
    TEST_ASSERT_TRUE_MESSAGE(buzzer.count() <= 2, "large time jump does not spam buzzer writes");
    TEST_ASSERT_TRUE(ba::kActiveToneMs > ba::kWarningCueToneMs);
}

void test_player_backwards_timestamp_is_safe(void) {
    tk::FakeClock clock;
    tk::RecordingLed<> led(clock);
    tk::RecordingBuzzer<> buzzer(clock);
    ba::PatternPlayer player(&led, &buzzer);
    player.select(ba::PatternId::Warning);

    tickAt(clock, player, 1000);
    const unsigned after_first = led.count();
    tickAt(clock, player, 500);  // time went backwards: hold step 0, no underflow
    TEST_ASSERT_EQUAL_UINT(after_first, led.count());
    assertRgb(ba::kLedWarning, led.last().rgb, "stays on the first step");
}

void test_player_size_within_budget(void) {
    TEST_ASSERT_LESS_THAN_MESSAGE(128, sizeof(ba::PatternPlayer),
                                  "the player shares the 1 KB feature budget");
}
