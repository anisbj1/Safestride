#include <unity.h>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/providers.h>

#include "../../../src/app/wiring/wire_input.h"

namespace {

class CountingSink final : public bw::ICommandSink {
public:
    bool push(const bw::Command& command) override {
        ++calls;
        last = command;
        return true;
    }
    uint8_t calls = 0;
    bw::Command last{};
};

bw::AlertStatus alertAt(bw::AlertLevel level) {
    bw::AlertStatus alert{};
    alert.level = level;
    return alert;
}

bw::Command ackAt(bw::CommandType type, bw::TimeMs t_ms) {
    bw::Command command{};
    command.type = type;
    command.t_ms = t_ms;
    return command;
}

void warning_ack_publishes_release_time_once() {
    bw::LatestValue<bw::TimeMs> passed_at;
    const bw::AlertStatus alert = alertAt(bw::AlertLevel::Warning);
    const bw::Command command = ackAt(bw::CommandType::AcknowledgeAlert, 123456);

    TEST_ASSERT_TRUE(f11::recordWakeCheckOnWarning(alert, command, passed_at));
    TEST_ASSERT_EQUAL_UINT32(1, passed_at.sequence());
    bw::TimeMs published = 0;
    TEST_ASSERT_TRUE(passed_at.get(published));
    TEST_ASSERT_EQUAL_UINT64(command.t_ms, published);
}

void warning_ack_still_routes_exactly_once() {
    bw::LatestValue<bw::TimeMs> passed_at;
    CountingSink sink;
    const bw::Command command = ackAt(bw::CommandType::AcknowledgeAlert, 456);
    const bw::AlertStatus alert = alertAt(bw::AlertLevel::Warning);
    TEST_ASSERT_TRUE(f11::pushWarningAck(&alert, command, passed_at, sink));
    TEST_ASSERT_EQUAL_UINT8(1, sink.calls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::CommandType::AcknowledgeAlert),
                            static_cast<uint8_t>(sink.last.type));
    TEST_ASSERT_EQUAL_UINT64(command.t_ms, sink.last.t_ms);
    TEST_ASSERT_EQUAL_UINT32(1, passed_at.sequence());
}

void missing_alert_does_not_publish_but_routes_ack_once() {
    bw::LatestValue<bw::TimeMs> passed_at;
    CountingSink sink;
    const bw::Command command = ackAt(bw::CommandType::AcknowledgeAlert, 789);
    TEST_ASSERT_TRUE(f11::pushWarningAck(nullptr, command, passed_at, sink));
    TEST_ASSERT_EQUAL_UINT8(1, sink.calls);
    TEST_ASSERT_FALSE(passed_at.hasValue());
}

void other_alert_levels_do_not_publish() {
    const bw::AlertLevel levels[] = {bw::AlertLevel::Normal, bw::AlertLevel::Active,
                                     bw::AlertLevel::Persistent};
    for (const bw::AlertLevel level : levels) {
        bw::LatestValue<bw::TimeMs> passed_at;
        TEST_ASSERT_FALSE(f11::recordWakeCheckOnWarning(
            alertAt(level), ackAt(bw::CommandType::AcknowledgeAlert, 99), passed_at));
        TEST_ASSERT_FALSE(passed_at.hasValue());
        TEST_ASSERT_EQUAL_UINT32(0, passed_at.sequence());
    }
}

void unrelated_command_does_not_publish() {
    bw::LatestValue<bw::TimeMs> passed_at;
    TEST_ASSERT_FALSE(f11::recordWakeCheckOnWarning(
        alertAt(bw::AlertLevel::Warning), ackAt(bw::CommandType::CancelAlert, 88), passed_at));
    TEST_ASSERT_FALSE(passed_at.hasValue());
}

}  // namespace

void register_wake_check_tests(void) {
    RUN_TEST(warning_ack_publishes_release_time_once);
    RUN_TEST(warning_ack_still_routes_exactly_once);
    RUN_TEST(missing_alert_does_not_publish_but_routes_ack_once);
    RUN_TEST(other_alert_levels_do_not_publish);
    RUN_TEST(unrelated_command_does_not_publish);
}
