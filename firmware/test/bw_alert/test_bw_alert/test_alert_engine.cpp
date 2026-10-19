// bw_alert - AlertEngine tests (F10 §3 outputs): the policy's decision is
// turned into a pattern, an AlertStatus and exactly one AlertRaised event per
// transition - plus the deferred-DI paths the wiring must survive (no sink,
// no actuators) and a fault-injected sink.

#include <unity.h>

#include <bw/alert/alert_engine.h>
#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_buzzer.h>
#include <bw/testkit/recording_led.h>

#include "alert_fixtures.h"
#include "recording_event_sink.h"

namespace ba = bw::alert;
using bw::AlertLevel;
using bw::Rgb;
using bw::TimeMs;
using f10test::assertLevel;
using f10test::assertPhase;
using f10test::Feed;
using f10test::makeInput;
using f10test::phaseOf;

namespace {

namespace tk = bw::testkit;

// The engine plus everything it drives. The FakeClock is advanced on every
// step, so the Recording* entries carry the same t_ms the engine was ticked
// with - the engine's own time comes from input.t_ms, the clock only stamps
// what the fakes record.
struct Harness {
    tk::FakeClock clock;
    tk::RecordingLed<> led;
    tk::RecordingBuzzer<> buzzer;
    f10test::RecordingEventSink sink;
    ba::AlertEngine engine;

    Harness() : led(clock), buzzer(clock) { engine.attachOutputs(&led, &buzzer, &sink); }

    ba::AlertOutput step(TimeMs t_ms, const Feed& feed = Feed{}) {
        clock.setMs(t_ms);
        return engine.step(makeInput(t_ms, feed));
    }
};

Feed drowsy() {
    Feed f;
    f.alertness = bw::AlertnessState::Drowsy;
    return f;
}

Feed risk() {
    Feed f;
    f.alertness = bw::AlertnessState::HighDrowsinessRisk;
    return f;
}

Feed noSnapshot() {
    Feed f;
    f.has_snapshot = false;
    return f;
}

void assertErr(bw::Err expected, bw::Err actual, const char* msg) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(actual), msg);
}

// An AlertOutput whose level did not change (the engine's equivalent of the
// policy's assertNoChange).
void assertSteady(const ba::AlertOutput& out, const char* msg) {
    TEST_ASSERT_FALSE_MESSAGE(out.level_changed, msg);
}

void assertLed(Rgb expected, const Rgb& actual, const char* msg) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.r, actual.r, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.g, actual.g, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(expected.b, actual.b, msg);
}

}  // namespace

// --- §3: decision ⇒ pattern ⇒ status ⇒ event -------------------------------

void test_engine_raise_publishes_alert_raised(void) {
    Harness h;

    const ba::AlertOutput boot = h.step(0, noSnapshot());
    assertSteady(boot, "boot must not raise anything");
    assertErr(bw::Err::Ok, boot.publish_err, "nothing to publish is Ok");

    const ba::AlertOutput raised = h.step(1000, drowsy());
    assertLevel(AlertLevel::Warning, raised.status, "Drowsy raises Warning");
    TEST_ASSERT_TRUE_MESSAGE(raised.level_changed, "the level changed");
    assertErr(bw::Err::Ok, raised.publish_err, "the sink accepted the event");
    TEST_ASSERT_EQUAL_UINT(1, h.sink.count());

    const bw::Event& e = h.sink.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::AlertRaised), static_cast<int>(e.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::AlertPhase::Raised), static_cast<int>(phaseOf(e)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Warning),
                          static_cast<int>(f10test::levelOfEvent(e)));
    TEST_ASSERT_EQUAL_UINT64(1000, e.t_start_ms);
    TEST_ASSERT_EQUAL_UINT8(ba::kAlertFeatureId, e.detector_id);
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, e.schema_version);
    TEST_ASSERT_EQUAL_UINT32(0, e.seq);  // assigned by F17 downstream
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ba::AlertPhase::Raised),
                            e.payload[ba::kAlertPayloadPhaseIndex]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertLevel::Warning),
                            e.payload[ba::kAlertPayloadLevelIndex]);
    // §6 D8: payload[2..7] are reserved and must stay zeroed.
    for (size_t i = ba::kAlertPayloadLevelIndex + 1; i < bw::Event::kMaxPayloadBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, e.payload[i], "the reserved payload tail is zero");
    }

    const ba::AlertOutput steady = h.step(1100, drowsy());
    assertSteady(steady, "a steady level emits no event");
    assertErr(bw::Err::Ok, steady.publish_err, "a level that did not change publishes nothing");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, h.sink.count(), "exactly one event per transition");
}

void test_engine_escalation_publishes_a_second_event(void) {
    Harness h;
    h.step(1000, drowsy());  // Warning

    const ba::AlertOutput escalated = h.step(1000 + ba::kWarningToActiveMs, drowsy());
    assertLevel(AlertLevel::Active, escalated.status, "the ladder beeps after kWarningToActiveMs");
    TEST_ASSERT_TRUE(escalated.level_changed);
    TEST_ASSERT_EQUAL_UINT(2, h.sink.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Active),
                          static_cast<int>(f10test::levelOfEvent(h.sink.last())));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::AlertPhase::Raised),
                          static_cast<int>(phaseOf(h.sink.last())));
}

void test_engine_ack_publishes_and_flags_the_status(void) {
    Harness h;
    h.step(1000, drowsy());

    Feed ack = drowsy();
    ack.acknowledge = true;
    const ba::AlertOutput acked = h.step(2000, ack);

    assertLevel(AlertLevel::Normal, acked.status, "ack silences at once");
    TEST_ASSERT_TRUE(acked.status.acknowledged);
    TEST_ASSERT_EQUAL_UINT(2, h.sink.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::AlertPhase::Acknowledged),
                          static_cast<int>(phaseOf(h.sink.last())));
    TEST_ASSERT_EQUAL_UINT32(1000, h.sink.last().t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, h.sink.last().duration_ms);
}

// --- §3 failure behaviour: the sink promise is observable -------------------

void test_engine_without_sink_reports_not_ready(void) {
    ba::AlertEngine engine;  // nothing attached yet (AppContext has no sink)
    const ba::AlertOutput raised = engine.step(makeInput(1000, drowsy()));

    assertLevel(AlertLevel::Warning, raised.status, "a missing sink never blocks the alert");
    TEST_ASSERT_TRUE(raised.level_changed);
    assertErr(bw::Err::NotReady, raised.publish_err,
              "an unmet event promise must be visible to the pump");
}

void test_engine_sink_error_is_not_ignored(void) {
    Harness h;
    h.sink.forceError(bw::Err::Full);

    const ba::AlertOutput raised = h.step(1000, drowsy());
    assertLevel(AlertLevel::Warning, raised.status, "a refusing sink does not stop the ladder");
    assertErr(bw::Err::Full, raised.publish_err, "the sink's error is reported, not dropped");
    TEST_ASSERT_EQUAL_UINT(0, h.sink.count());  // Err::Full: nothing written
}

// --- §6 D7: the mode profile drives the output too -------------------------

void test_engine_night_profile_dims_the_led(void) {
    Harness h;
    Feed night = drowsy();
    night.mode = bw::OperatingMode::Night;

    const ba::AlertOutput out = h.step(1000, night);
    assertLevel(AlertLevel::Warning, out.status, "Night still alerts");
    TEST_ASSERT_EQUAL_UINT8(60, h.engine.policy().profile().led_gain_pct);

    const Rgb expected{static_cast<uint8_t>(255 * 60 / 100), static_cast<uint8_t>(128 * 60 / 100),
                       0};
    assertLed(expected, h.led.last().rgb, "the Night profile dims every channel");
}

// --- §7: the pattern follows the level -------------------------------------

void test_engine_pattern_follows_the_level(void) {
    Harness h;

    h.step(0, Feed{});  // Awake: Off
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Off),
                          static_cast<int>(h.engine.player().current()));
    assertLed(ba::kLedOff, h.led.last().rgb, "no alert means a dark LED");

    h.step(1000, drowsy());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Warning),
                          static_cast<int>(h.engine.player().current()));
    assertLed(ba::kLedWarning, h.led.last().rgb, "Warning lights the amber pattern");
    TEST_ASSERT_TRUE_MESSAGE(h.buzzer.last().on, "Warning onset plays the two-beep cue");

    h.step(2000, risk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Active),
                          static_cast<int>(h.engine.player().current()));
    assertLed(ba::kLedActive, h.led.last().rgb, "Active lights the red pattern");
    TEST_ASSERT_TRUE(h.buzzer.last().on);
}

// --- §10: stale input stops the alert and the pattern ----------------------

void test_engine_stale_stops_and_reports(void) {
    Harness h;
    h.step(1000, drowsy());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Warning),
                          static_cast<int>(h.engine.player().current()));

    const ba::AlertOutput stale = h.step(1000 + ba::kStateStaleMs + 1, noSnapshot());

    assertLevel(AlertLevel::Normal, stale.status, "stale input de-escalates");
    TEST_ASSERT_TRUE_MESSAGE(
        f10test::hasReason(stale.status.reasons, bw::ReasonCode::SensorUnavailable),
        "AlertStatus says SensorUnavailable");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::AlertPhase::StateStale),
                          static_cast<int>(phaseOf(h.sink.last())));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ba::PatternId::Off),
                          static_cast<int>(h.engine.player().current()));
    TEST_ASSERT_FALSE_MESSAGE(h.buzzer.last().on, "the buzzer stops with the alert");
    assertLed(ba::kLedOff, h.led.last().rgb, "and the LED goes dark with it");
}

// --- budget (F10 §11: RAM < 1 KB) -------------------------------------------

void test_engine_size_within_budget(void) {
    TEST_ASSERT_LESS_THAN_MESSAGE(1024, sizeof(ba::AlertEngine),
                                  "the whole feature must fit in its 1 KB RAM budget");
}
