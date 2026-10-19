// Real F10 command drain -> F24 SPSC/pump/provider -> pure F07 pump.
#include "../../src/app/wiring/wire_alert.cpp"
#include "../../src/app/wiring/wire_modes.cpp"
#include "../../src/app/wiring/wire_state.cpp"

#include <bw/modes/mode_config.h>
#include <bw/testkit/fake_clock.h>
#include <unity.h>

namespace {
// [DERIVED] Match the existing service cadence; no independent timing policy.
constexpr bw::TimeMs kModeTestTickMs = bw::modes::kModePumpPeriodMs;

class LowBattery final : public bw::IBatteryMonitor {
public:
    bw::BatteryReading read() override {
        bw::BatteryReading out{};
        out.valid = true;
        out.percent_estimate = bw::modes::kLowPowerEnterPercent;
        return out;
    }
};

void enqueue(app::AppContext& ctx, bw::CommandType type, bw::OperatingMode mode) {
    bw::Command command{};
    command.type = type;
    command.arg0 = static_cast<uint32_t>(mode);
    TEST_ASSERT_EQUAL(bw::Err::Ok, ctx.commands.push(command));
}

void expectMode(bw::state::StatePump& pump, bw::TimeMs now, bw::OperatingMode mode,
                bw::PowerProfile power) {
    const auto out = pump.tick(now);
    TEST_ASSERT_EQUAL(mode, out.snapshot.mode);
    TEST_ASSERT_EQUAL(power, out.snapshot.power);
    // Mode metadata cannot fabricate evidence or bypass the missing-data gate.
    TEST_ASSERT_EQUAL(bw::AlertnessState::Unknown, out.snapshot.alertness);
}
}  // namespace

void test_mode_provider_absence_and_unpublished_keep_original_defaults() {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f24::wireModesInit(ctx, nullptr);
    bw::state::StatePump pump;
    expectMode(pump, 0, bw::OperatingMode::Day, bw::PowerProfile::Normal);
    pump.ports().mode_provider = f24::modeProvider();
    // This case runs before the first F24 pump in this process.
    expectMode(pump, 0, bw::OperatingMode::Day, bw::PowerProfile::Normal);
}

void test_mode_commands_relay_to_real_provider_and_state_pump() {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    LowBattery battery;
    ctx.clock = &clock;
    f24::wireModesInit(ctx, &battery);
    bw::state::StatePump pump;
    bw::LatestValue<bw::StateSnapshot> snapshot;
    pump.ports().snapshot = &snapshot;
    pump.ports().mode_provider = f24::modeProvider();

    // Each supported command travels through ctx.commands, not directly F24.
    const bw::CommandType commands[] = {bw::CommandType::SetMode, bw::CommandType::StartNap,
                                        bw::CommandType::EnterResearch,
                                        bw::CommandType::ExitResearch};
    const bw::OperatingMode expected[] = {bw::OperatingMode::Night, bw::OperatingMode::Nap,
                                          bw::OperatingMode::Research, bw::OperatingMode::Nap};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        clock.advanceMs(kModeTestTickMs);
        enqueue(ctx, commands[i], bw::OperatingMode::Night);
        enqueue(ctx, bw::CommandType::AcknowledgeAlert, bw::OperatingMode::Day);
        bw::alert::PolicyInput input{};
        TEST_ASSERT_EQUAL_UINT32(0, f10::drainCommands(ctx, input, &f24::modeCommandSink()));
        TEST_ASSERT_TRUE(input.acknowledge);
        TEST_ASSERT_FALSE(input.cancel);
        TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
        expectMode(pump, clock.nowMs(), expected[i], bw::PowerProfile::LowPower);
        bw::StateSnapshot published{};
        TEST_ASSERT_TRUE(snapshot.get(published));
        TEST_ASSERT_EQUAL(expected[i], published.mode);
        TEST_ASSERT_EQUAL(bw::PowerProfile::LowPower, published.power);
        // Draining again must not repeat an alert pulse or relay a command.
        bw::alert::PolicyInput next{};
        TEST_ASSERT_EQUAL_UINT32(0, f10::drainCommands(ctx, next, &f24::modeCommandSink()));
        TEST_ASSERT_FALSE(next.acknowledge);
    }
    pump.ports().mode_provider = nullptr;
    expectMode(pump, clock.nowMs(), bw::OperatingMode::Day, bw::PowerProfile::Normal);
    // Wiring holds these only for task lifetime; detach stack fakes after use.
    app::AppContext detached{};
    f24::wireModesInit(detached, nullptr);
}

void test_full_or_absent_mode_relay_does_not_lose_alert_pulses() {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f24::wireModesInit(ctx, nullptr);
    for (size_t i = 0; i < bw::modes::kModeCommandQueueDepth; ++i) {
        enqueue(ctx, bw::CommandType::SetMode, bw::OperatingMode::Day);
        bw::alert::PolicyInput in{};
        TEST_ASSERT_EQUAL_UINT32(0, f10::drainCommands(ctx, in, &f24::modeCommandSink()));
    }
    enqueue(ctx, bw::CommandType::SetMode, bw::OperatingMode::Night);
    enqueue(ctx, bw::CommandType::CancelAlert, bw::OperatingMode::Day);
    enqueue(ctx, bw::CommandType::AcknowledgeAlert, bw::OperatingMode::Day);
    bw::alert::PolicyInput in{};
    TEST_ASSERT_EQUAL_UINT32(1, f10::drainCommands(ctx, in, &f24::modeCommandSink()));
    TEST_ASSERT_TRUE(in.cancel);
    TEST_ASSERT_TRUE(in.acknowledge);
    // Drain the refused queue to leave no inherited command for later tests.
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
    enqueue(ctx, bw::CommandType::SetMode, bw::OperatingMode::Night);
    enqueue(ctx, bw::CommandType::CancelAlert, bw::OperatingMode::Day);
    bw::alert::PolicyInput absent{};
    TEST_ASSERT_EQUAL_UINT32(1, f10::drainCommands(ctx, absent, nullptr));
    TEST_ASSERT_TRUE(absent.cancel);
    app::AppContext detached{};
    f24::wireModesInit(detached, nullptr);
}
