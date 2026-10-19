#pragma once

// Shared fixtures for the bw_alert / F10 suite: one way to build a
// StateSnapshot and one way to run a policy step, so every test reads as a
// (t_ms, input)"). Private to this test folder (host-testing guidance: shared
// fakes belong to bw_testkit, private helpers belong to the feature's tests).

#include <unity.h>

#include <bw/alert/alert_policy.h>
#include <bw/state.h>
#include <bw/time.h>

namespace ba = bw::alert;

namespace f10test {

// One scripted step: what the wiring would have found at `t_ms`.
struct Feed {
    bool has_snapshot = true;
    bw::AlertnessState alertness = bw::AlertnessState::Awake;
    bw::WearState wear = bw::WearState::Worn;
    bw::OperatingMode mode = bw::OperatingMode::Day;
    bool acknowledge = false;
    bool cancel = false;
    ba::EventNotification notification{};
};

inline bw::StateSnapshot makeState(bw::TimeMs t_ms, const Feed& feed) {
    bw::StateSnapshot s{};
    s.t_ms = t_ms;
    s.alertness = feed.alertness;
    s.wear = feed.wear;
    s.activity = bw::ActivityContext::Sitting;
    s.quality = bw::SourceQuality{bw::DataQuality::Good, bw::DataQuality::Good};
    s.mode = feed.mode;
    s.power = bw::PowerProfile::Normal;
    s.in_state_ms = 1000;
    s.confidence = 0.75f;
    s.reasons = ba::reasonBit(bw::ReasonCode::SustainedLowMotion);
    return s;
}

// Builds the input the wiring would have produced at `t_ms`. The snapshot's
// own t_ms always equals `t_ms` (a fresh StateTask output), so staleness only
// appears when has_snapshot is false - exactly how a stalled producer looks to
// the engine. Shared by the policy and the engine tests.
inline ba::PolicyInput makeInput(bw::TimeMs t_ms, const Feed& feed = Feed{}) {
    ba::PolicyInput in{};
    in.t_ms = t_ms;
    in.has_state = feed.has_snapshot;
    if (feed.has_snapshot) {
        in.state = makeState(t_ms, feed);
    }
    in.notification = feed.notification;
    in.acknowledge = feed.acknowledge;
    in.cancel = feed.cancel;
    return in;
}

// Runs one policy evaluation at `t_ms`.
inline ba::PolicyOutput step(ba::AlertPolicy& policy, bw::TimeMs t_ms, const Feed& feed = Feed{}) {
    return policy.evaluate(makeInput(t_ms, feed));
}

inline void assertLevel(bw::AlertLevel expected, const bw::AlertStatus& status, const char* msg) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(status.level), msg);
}

inline void assertLevel(bw::AlertLevel expected, const ba::PolicyOutput& out, const char* msg) {
    assertLevel(expected, out.status, msg);
}

inline ba::AlertPhase phaseOf(const bw::Event& e) {
    return static_cast<ba::AlertPhase>(e.payload[ba::kAlertPayloadPhaseIndex]);
}

inline bw::AlertLevel levelOfEvent(const bw::Event& e) {
    return static_cast<bw::AlertLevel>(e.payload[ba::kAlertPayloadLevelIndex]);
}

inline void assertPhase(ba::AlertPhase expected, const ba::PolicyOutput& out, const char* msg) {
    TEST_ASSERT_TRUE_MESSAGE(out.level_changed, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(phaseOf(out.event)),
                                  msg);
}

inline void assertNoChange(const ba::PolicyOutput& out, const char* msg) {
    TEST_ASSERT_FALSE_MESSAGE(out.level_changed, msg);
}

// A ReasonMask containing exactly `code`.
inline bool hasReason(bw::ReasonMask mask, bw::ReasonCode code) {
    return (mask & ba::reasonBit(code)) != 0;
}

}  // namespace f10test
