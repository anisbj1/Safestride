// bw_modes — wiring compile + pump test (spec §7.2, DoD §14, health H1–H6).
//
// wire_modes.cpp is compiled for the HOST by including it into this
// translation unit (same-TU include, so the f24:: statics are observable —
// test_wire_ble_compile.cpp precedent). On the host there is no hardware:
// the pump runs the same logic as the device against a scripted FakeClock
// and a fake battery monitor — this is the "pure pump entry point,
// host-testable without hardware" seam the integrator's P4 wiring will
// pace at bw::modes::kModePumpPeriodMs (2 s, G1 ruling 2).
//
// The f24:: wiring state is file-static and shared: register these tests
// LAST in main.cpp, in file order.

#include "../../../src/app/wiring/wire_modes.cpp"

#include <bw/containers/spsc_queue.h>
#include <bw/modes/mode_config.h>
#include <bw/testkit/fake_clock.h>
#include <unity.h>

#include "mode_test_helpers.h"

using bw::testkit::FakeClock;
using bw_modes_test::batteryAt;
using bw_modes_test::makeCommand;
using bw_modes_test::noBattery;

namespace {

// Private fake (allowed under test/bw_modes/): scriptable reading, exactly
// like the device's IBatteryMonitor but deterministic.
class FakeBattery final : public bw::IBatteryMonitor {
public:
    bw::BatteryReading read() override { return reading; }
    bw::BatteryReading reading{};
};

FakeClock s_clock;
FakeBattery s_battery;

}  // namespace

// Order-dependent by design: one shared f24:: static state, exactly as the
// composition root sees it (registered in this order in main.cpp).

// Spec §10 / wire_modes.h — before wire(): every seam is honest about
// being unavailable; nothing is fabricated.
void test_wire_getters_honest_before_wire(void) {
    bw::ModeProfile p{};
    TEST_ASSERT_FALSE(f24::modeProfile(p));
    TEST_ASSERT_NULL(f24::modeProvider());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotReady),
                          static_cast<int>(f24::modePumpOnce().err));
}

// Wire (twice — idempotent, the manager instance survives) and pump once:
// the profile is published with the boot state (Day/Normal, unknown time).
void test_wire_idempotent_init_and_first_pump(void) {
    app::AppContext ctx{};
    s_clock.setMs(1234);
    ctx.clock = &s_clock;
    f24::wireModesInit(ctx, &s_battery);
    f24::wireModesInit(ctx, &s_battery);  // second call: idempotent, no crash

    TEST_ASSERT_NOT_NULL(f24::modeProvider());
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());

    bw::ModeProfile p{};
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(p.mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal), static_cast<int>(p.power));
    TEST_ASSERT_EQUAL_UINT8(100, p.ui_brightness_pct);  // §8 day row
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::AlertLevel::Persistent),
                          static_cast<int>(p.alert_max_level));
    TEST_ASSERT_FALSE(p.wall_time_known);  // FakeClock starts at unix 0
    TEST_ASSERT_EQUAL_UINT64(1234, p.t_ms);
}

// The command seam: a router-side push lands through one pump tick.
void test_wire_command_sink_set_mode_via_pump(void) {
    bw::Command cmd =
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Night));
    TEST_ASSERT_TRUE(f24::modeCommandSink().push(cmd));
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());

    bw::ModeProfile p{};
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night), static_cast<int>(p.mode));
    TEST_ASSERT_EQUAL_UINT8(40, p.ui_brightness_pct);  // §8 night row
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::AlertLevel::Active),
                          static_cast<int>(p.alert_max_level));
}

// Bounded drain (D9): a full burst of kModeCommandQueueDepth commands is
// refused-then-counted on overflow, and exactly kMaxCommandsPerPump are
// applied per pump tick — leftovers wait for the next tick.
void test_wire_queue_full_refused_and_bounded_drain(void) {
    namespace modes = bw::modes;

    // Fill the queue: [0..3] applied by pump 1, [4..7] by pump 2.
    const uint32_t burst[] = {
        static_cast<uint32_t>(bw::OperatingMode::Calibration),
        static_cast<uint32_t>(bw::OperatingMode::Day),
        static_cast<uint32_t>(bw::OperatingMode::Nap),
        static_cast<uint32_t>(bw::OperatingMode::Day),
        static_cast<uint32_t>(bw::OperatingMode::Night),
        static_cast<uint32_t>(bw::OperatingMode::Day),
        static_cast<uint32_t>(bw::OperatingMode::Calibration),
        static_cast<uint32_t>(bw::OperatingMode::Night),
    };
    for (const uint32_t to : burst) {
        TEST_ASSERT_TRUE(f24::modeCommandSink().push(makeCommand(bw::CommandType::SetMode, to)));
    }
    // The 9th push exceeds kModeCommandQueueDepth = 8: refused (CR-004 Full
    // semantics) and counted by the queue's drops() — pinned here, not just
    // claimed: reverting the sink's Err::Full forwarding fails this assert.
    TEST_ASSERT_FALSE(f24::modeCommandSink().push(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Nap))));
    TEST_ASSERT_EQUAL_UINT32(1, f24::s_commands.drops());

    bw::ModeProfile p{};
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day),
                          static_cast<int>(p.mode));  // burst[3], pump 1

    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(p.mode));  // burst[7], pump 2
}

// Battery axis through the wiring: the pump folds the monitor reading into
// every step, and the published profile carries the power axis.
void test_wire_battery_drives_power_axis(void) {
    s_battery.reading = batteryAt(10.0f);  // below the enter threshold
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());

    bw::ModeProfile p{};
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower), static_cast<int>(p.power));
    // Mode untouched by the battery axis (Night from the previous test).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night), static_cast<int>(p.mode));
    TEST_ASSERT_EQUAL_UINT8(40, p.ui_brightness_pct);  // night is under the cap

    s_battery.reading = noBattery();  // unknown ⇒ Normal (§10)
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal), static_cast<int>(p.power));
}

// Wall time arrives (phone SetTime) while the device sits in the night
// window: wall_time_known flips true and the initial sync Day→Night genuinely
// fires through the wiring seam (the manager is parked in Day first, so this
// is a real crossing, not the already-Night no-op).
void test_wire_wall_time_known_activates_auto(void) {
    // Park in Day with an explicit command (previous test left the mode Night).
    TEST_ASSERT_TRUE(f24::modeCommandSink().push(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day))));
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());
    bw::ModeProfile parked{};
    TEST_ASSERT_TRUE(f24::modeProfile(parked));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day), static_cast<int>(parked.mode));

    // Wall time appears at 23:00 — inside the night window: the first tick
    // with known time performs the boot-style initial sync Day→Night.
    s_clock.setUnixSeconds(bw_modes_test::unixAt(23, 0, 0));
    TEST_ASSERT_TRUE(f24::modePumpOnce().ok());

    bw::ModeProfile p{};
    TEST_ASSERT_TRUE(f24::modeProfile(p));
    TEST_ASSERT_TRUE(p.wall_time_known);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night), static_cast<int>(p.mode));
}

// The IModeProvider interface (CR-027) hands out the same snapshot the
// getter does — the seam F07 will stamp StateSnapshot.mode/power from.
void test_wire_provider_interface_matches_mailbox(void) {
    const bw::IModeProvider* provider = f24::modeProvider();
    TEST_ASSERT_NOT_NULL(provider);

    bw::ModeProfile via_interface{};
    bw::ModeProfile via_getter{};
    TEST_ASSERT_TRUE(provider->latest(via_interface));
    TEST_ASSERT_TRUE(f24::modeProfile(via_getter));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(via_getter.mode), static_cast<int>(via_interface.mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(via_getter.power),
                          static_cast<int>(via_interface.power));
    TEST_ASSERT_EQUAL_UINT8(via_getter.ui_brightness_pct, via_interface.ui_brightness_pct);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(via_getter.alert_max_level),
                          static_cast<int>(via_interface.alert_max_level));
    TEST_ASSERT_EQUAL_INT(via_getter.wall_time_known, via_interface.wall_time_known);
}
