// bw_modes — power policy suite (F24 spec §9 cases 13–16, §6 D6) plus the
// §8 profile table over every (mode, power) combination. Power is an
// ORTHOGONAL axis: it never sets `changed` and never moves the mode.

#include <cmath>

#include <bw/modes/mode_manager.h>
#include <bw/modes/mode_profile_table.h>
#include <unity.h>

#include "mode_test_helpers.h"

using bw_modes_test::batteryAt;
using bw_modes_test::chargingBattery;
using bw_modes_test::commandAt;
using bw_modes_test::makeCommand;
using bw_modes_test::noBattery;
using bw_modes_test::settleWithBattery;

namespace {

void driveToLowPower(bw::modes::ModeManager& mgr) {
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleWithBattery(0, batteryAt(10.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));
    TEST_ASSERT_FALSE(r.value.changed);  // power-only is never a mode change
}

}  // namespace

// Spec §9 case 13 — hysteresis boundaries: enter ≤ 15 %, exit ≥ 25 %, hold
// in between.
void test_power_enter_exit_boundaries(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};

    // 16 % ⇒ Normal (above enter).
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleWithBattery(0, batteryAt(16.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r.value.power));

    // Exactly 15 % ⇒ LowPower (boundary is inclusive).
    r = mgr.step(settleWithBattery(0, batteryAt(15.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));

    // 16 % again — inside the band: holds LowPower (no flapping).
    r = mgr.step(settleWithBattery(0, batteryAt(16.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));

    // 24 % ⇒ still LowPower (just below exit).
    r = mgr.step(settleWithBattery(0, batteryAt(24.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));

    // Exactly 25 % ⇒ Normal (exit boundary inclusive).
    r = mgr.step(settleWithBattery(0, batteryAt(25.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r.value.power));

    // 14 % ⇒ straight back to LowPower.
    r = mgr.step(settleWithBattery(0, batteryAt(14.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));
}

// Spec §9 case 14 — valid==false ⇒ Normal immediately, even mid-LowPower.
void test_power_invalid_battery_forces_normal(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    driveToLowPower(mgr);

    const bw::Result<bw::modes::ModeOutput> r = mgr.step(settleWithBattery(0, noBattery()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r.value.power));
    // Mode untouched: Night+unknown battery would still be Night.
    (void)mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Night))));
    driveToLowPower(mgr);
    const bw::Result<bw::modes::ModeOutput> r2 = mgr.step(settleWithBattery(0, noBattery()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r2.value.mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r2.value.power));
}

// Spec §9 case 15 — charging_known && charging ⇒ Normal even at a low
// percentage; the override needs the charging flag to be actually true.
void test_power_charging_overrides_low_battery(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleWithBattery(0, chargingBattery(5.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r.value.power));

    // charging known but NOT charging at 5 % ⇒ LowPower like any draining
    // battery (the override is about mains, not about the flag existing).
    r = mgr.step(settleWithBattery(0, batteryAt(5.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));
}

// Spec §9 case 16 — Night + LowPower coexist (00 §6 axes), and the §8 table
// covers every (mode, power) combination. This is also DoD §14 row 4.
void test_profile_values_for_all_mode_power_combos(void) {
    // Manager-level: the two axes move independently in one step.
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    const bw::Result<bw::modes::ModeOutput> r = mgr.step(bw_modes_test::commandWithBattery(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Night)),
        batteryAt(10.0f)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r.value.mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(r.value.power));
    TEST_ASSERT_TRUE(r.value.changed);  // the MODE moved; power is incidental

    // Table-level: all 10 combinations, exactly per spec §8.
    struct ComboRow {
        bw::OperatingMode mode;
        bw::PowerProfile power;
        uint8_t brightness;
        bw::AlertLevel alert;
    };
    constexpr ComboRow kCombos[] = {
        {bw::OperatingMode::Day, bw::PowerProfile::Normal, 100, bw::AlertLevel::Persistent},
        {bw::OperatingMode::Day, bw::PowerProfile::LowPower, 50, bw::AlertLevel::Persistent},
        {bw::OperatingMode::Night, bw::PowerProfile::Normal, 40, bw::AlertLevel::Active},
        {bw::OperatingMode::Night, bw::PowerProfile::LowPower, 40, bw::AlertLevel::Active},
        {bw::OperatingMode::Nap, bw::PowerProfile::Normal, 30, bw::AlertLevel::Active},
        {bw::OperatingMode::Nap, bw::PowerProfile::LowPower, 30, bw::AlertLevel::Active},
        {bw::OperatingMode::Calibration, bw::PowerProfile::Normal, 80, bw::AlertLevel::Warning},
        {bw::OperatingMode::Calibration, bw::PowerProfile::LowPower, 50, bw::AlertLevel::Warning},
        {bw::OperatingMode::Research, bw::PowerProfile::Normal, 80, bw::AlertLevel::Warning},
        {bw::OperatingMode::Research, bw::PowerProfile::LowPower, 50, bw::AlertLevel::Warning},
    };
    for (const ComboRow& c : kCombos) {
        const bw::modes::ModeProfileHints h = bw::modes::hintsFor(c.mode, c.power);
        TEST_ASSERT_EQUAL_UINT8(c.brightness, h.ui_brightness_pct);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(c.alert), static_cast<int>(h.alert_max_level));
    }
}

// Spec §10 honesty extra (added at IMPLEMENT): a non-finite percentage is
// treated exactly like an invalid reading — a fabricated number must never
// drive the profile (00 §1). Reverting the isfinite() guard in
// updatePower() fails this test (NaN comparisons are false → the guard-less
// code would HOLD LowPower forever).
void test_power_nonfinite_percent_forces_normal(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    driveToLowPower(mgr);

    const bw::Result<bw::modes::ModeOutput> r = mgr.step(settleWithBattery(0, batteryAt(NAN)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::Normal),
                          static_cast<int>(r.value.power));
}
