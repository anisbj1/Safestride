// bw_modes — auto day/night scheduler suite (F24 spec §9 cases 7–12, §6 D4).
// Scripted wall-clock timelines: the D4 rule fires ONLY on a window crossing
// with known wall time, and never outside Day/Night.

#include <bw/modes/mode_manager.h>
#include <unity.h>

#include "mode_test_helpers.h"

using bw_modes_test::commandAt;
using bw_modes_test::makeCommand;
using bw_modes_test::settleAt;
using bw_modes_test::unixAt;

namespace {

void assertMode(const bw::modes::ModeOutput& out, bw::OperatingMode mode) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(mode), static_cast<int>(out.mode));
}

}  // namespace

// Spec §9 case 7 — 21:59:59 stays Day; 22:00:00 flips Night (AutoSchedule).
void test_schedule_flips_to_night_at_220000(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleAt(unixAt(21, 59, 59)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Day);
    TEST_ASSERT_TRUE(r.value.wall_time_known);

    r = mgr.step(settleAt(unixAt(22, 0, 0)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day),
                          static_cast<int>(r.value.previous_mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::AutoSchedule),
                          static_cast<int>(r.value.reason));
}

// Spec §9 case 8 — 06:59:59 Night → 07:00:00 Day; midnight wrap stays Night.
void test_schedule_day_at_070000_and_midnight_wrap(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    // Reach Night via the 22:00 crossing.
    (void)mgr.step(settleAt(unixAt(21, 59, 59)));
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleAt(unixAt(22, 0, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r.value.mode));

    r = mgr.step(settleAt(unixAt(6, 59, 59)));
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);

    r = mgr.step(settleAt(unixAt(7, 0, 0)));
    TEST_ASSERT_TRUE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Day);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::AutoSchedule),
                          static_cast<int>(r.value.reason));

    // Midnight wrap: cross into night at 22:00 (last_scheduled := Night), so
    // 23:00 has no new edge; 23:59:59 and 00:00:00 stay Night (the schedule is
    // still "night" — no new crossing at midnight).
    bw::modes::ModeManager wrap{bw::modes::ModeConfig{}};
    r = wrap.step(settleAt(unixAt(21, 30, 0)));  // pre-window: last_scheduled := Day
    TEST_ASSERT_FALSE(r.value.changed);
    (void)wrap.step(settleAt(unixAt(22, 0, 0)));  // 22:00 crossing -> Night
    r = wrap.step(settleAt(unixAt(23, 0, 0)));    // inside window: no edge
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
    r = wrap.step(settleAt(unixAt(23, 59, 59)));
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
    r = wrap.step(settleAt(unixAt(0, 0, 0)));  // midnight: still night
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
}

// Spec §9 case 9 — unix_s == 0 (wall time unknown): no auto switching ever,
// the last explicit mode stands; wall_time_known is false.
void test_schedule_never_fires_without_wall_time(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Night))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r.value.mode));

    // Many ticks with unknown time — the 22:00 boundary is "crossed" in real
    // time but invisible to the manager: it must never guess (00 §4).
    for (int i = 0; i < 10; ++i) {
        r = mgr.step(settleAt(0));
        TEST_ASSERT_FALSE(r.value.changed);
        assertMode(r.value, bw::OperatingMode::Night);
        TEST_ASSERT_FALSE(r.value.wall_time_known);
    }
}

// Spec §9 case 10 — auto is suppressed in Nap/Calibration/Research across the
// boundary; the edge detector still tracks the schedule, so nothing fires
// retroactively when the user comes back to Day.
void test_schedule_suppressed_outside_day_night(void) {
    const bw::OperatingMode guarded[] = {bw::OperatingMode::Nap, bw::OperatingMode::Calibration,
                                         bw::OperatingMode::Research};
    for (const bw::OperatingMode mode : guarded) {
        bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
        if (mode == bw::OperatingMode::Research) {
            (void)mgr.step(commandAt(makeCommand(bw::CommandType::EnterResearch)));
        } else {
            (void)mgr.step(
                commandAt(makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(mode))));
        }
        TEST_ASSERT_EQUAL_INT(static_cast<int>(mode), static_cast<int>(mgr.mode()));

        (void)mgr.step(settleAt(unixAt(21, 59, 59)));
        bw::Result<bw::modes::ModeOutput> r = mgr.step(settleAt(unixAt(22, 0, 0)));
        TEST_ASSERT_FALSE(r.value.changed);  // suppressed
        assertMode(r.value, mode);
    }
}

// Spec §9 case 11 (D4 override case) — explicit SetMode(Day) at 23:00 with
// known time: Day is NOT undone until the next actual crossing (07:00), and
// that crossing to an already-matching schedule is a no-op.
void test_explicit_mode_survives_until_next_crossing(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    (void)mgr.step(settleAt(unixAt(21, 0, 0)));
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleAt(unixAt(22, 0, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(r.value.mode));

    // User overrides at 23:00 — the schedule is night, but the window already
    // crossed at 22:00, so no crossing fires on this tick.
    r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day)),
        unixAt(23, 0, 0)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Day);

    // Day persists through the rest of the night window.
    const bw::UnixSeconds night_points[] = {unixAt(23, 30, 0), unixAt(0, 0, 0), unixAt(3, 0, 0),
                                            unixAt(6, 59, 59)};
    for (const bw::UnixSeconds t : night_points) {
        r = mgr.step(settleAt(t));
        TEST_ASSERT_FALSE(r.value.changed);
        assertMode(r.value, bw::OperatingMode::Day);
    }

    // 07:00 crossing: the schedule becomes Day — which is already the current
    // mode, so the edge fires but the transition is a no-op.
    r = mgr.step(settleAt(unixAt(7, 0, 0)));
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Day);
}

// Spec §9 case 12 — boot with known time already inside the night window:
// the FIRST tick syncs Day→Night (last_scheduled starts at Day).
void test_boot_inside_night_window_initial_sync(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    bw::Result<bw::modes::ModeOutput> r = mgr.step(settleAt(unixAt(23, 0, 0)));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::AutoSchedule),
                          static_cast<int>(r.value.reason));

    // And it is stable: the next tick has no new crossing.
    r = mgr.step(settleAt(unixAt(23, 0, 2)));  // a 2 s ServicePump tick later
    TEST_ASSERT_FALSE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
}

// Documented edge (mode_manager.h step() contract): when a command transition
// and a schedule crossing land on the SAME tick, both apply in order and the
// output reports the net effect with the final transition's reason.
void test_command_and_crossing_same_tick_reports_net_effect(void) {
    bw::modes::ModeManager mgr{bw::modes::ModeConfig{}};
    // SetMode(Day) exactly on the 22:00:00 tick: the command runs first…
    bw::Result<bw::modes::ModeOutput> r = mgr.step(commandAt(
        makeCommand(bw::CommandType::SetMode, static_cast<uint32_t>(bw::OperatingMode::Day)),
        unixAt(22, 0, 0)));
    // …then the fresh boundary flips Night: previous_mode is the step's START
    // (Day), the reason is the final transition's (AutoSchedule).
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.value.changed);
    assertMode(r.value, bw::OperatingMode::Night);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Day),
                          static_cast<int>(r.value.previous_mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::modes::ModeChangeReason::AutoSchedule),
                          static_cast<int>(r.value.reason));
}
