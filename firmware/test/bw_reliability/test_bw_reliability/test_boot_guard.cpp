// bw_reliability — BootGuard tests (spec §16.8 item 3): streak arithmetic,
// the safe-mode decision, the survival exit (row 7/§16.4) and the row 8
// reset-reason -> FaultCode map. The KV-backed "missing/corrupt record" cases
// are wiring-level and live in test_wire_reliability_compile.cpp (same TU as
// wire_reliability.cpp, where applyBootRecord() is observable).

#include <bw/fault.h>
#include <bw/reliability/boot_guard.h>
#include <bw/reliability/reliability_config.h>

#include <unity.h>

namespace {

using bw::reliability::BootGuard;
using bw::reliability::ResetReason;

}  // namespace

// D10's streak rule: THIS boot counts, so persisted 0 -> 1 (normal),
// persisted 1 -> 2 (normal), persisted 2 -> 3 => safe mode (the 3rd fast
// boot). "2 fast boots stay normal, the 3rd is safe mode" (§16.3).
void test_boot_two_fast_boots_normal_third_safe_mode(void) {
    BootGuard g;
    g.onBoot(0, ResetReason::Software);
    TEST_ASSERT_EQUAL_UINT8(1, g.streak());
    TEST_ASSERT_FALSE(g.safeMode());

    g.onBoot(1, ResetReason::Software);
    TEST_ASSERT_EQUAL_UINT8(2, g.streak());
    TEST_ASSERT_FALSE(g.safeMode());

    g.onBoot(2, ResetReason::TaskWatchdog);
    TEST_ASSERT_EQUAL_UINT8(3, g.streak());
    TEST_ASSERT_TRUE(g.safeMode());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ResetReason::TaskWatchdog),
                          static_cast<int>(g.reason()));
}

// D10 exit: surviving kBootLoopBootLimitMs clears the streak (persisted by
// the caller) exactly once; safe_mode itself only changes on the NEXT boot —
// no live unwiring (G1 ruling 2: detection + publication only).
void test_boot_survival_clears_streak_once_safe_mode_stays(void) {
    BootGuard g;
    g.onBoot(2, ResetReason::Panic);
    TEST_ASSERT_TRUE(g.safeMode());

    TEST_ASSERT_FALSE(g.tickSurvived(bw::reliability::kBootLoopBootLimitMs - 1, 0));
    TEST_ASSERT_EQUAL_UINT8(3, g.streak());  // still inside the danger window

    TEST_ASSERT_TRUE(g.tickSurvived(bw::reliability::kBootLoopBootLimitMs, 0));
    TEST_ASSERT_EQUAL_UINT8(0, g.streak());
    TEST_ASSERT_TRUE(g.safeMode());  // stays as decided at boot

    // Exactly once: a later call must not report a second "persist now".
    TEST_ASSERT_FALSE(g.tickSurvived(bw::reliability::kBootLoopBootLimitMs + 1000, 0));
}

// A streak must saturate instead of wrapping u8 into "safe" territory.
void test_boot_streak_saturates(void) {
    BootGuard g;
    g.onBoot(255, ResetReason::Panic);
    TEST_ASSERT_EQUAL_UINT8(bw::reliability::kMaxRebootsInWindow, g.streak());
    TEST_ASSERT_TRUE(g.safeMode());
}

// Row 8: TWDT/INT-WDT => WatchdogReset, brownout => Brownout; every other
// reason (power-on, software, panic, deep sleep, unknown) => None — recorded
// in the crash record, never an invented code (append-only registry).
void test_boot_reset_reason_maps_to_row8_codes(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::WatchdogReset),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::TaskWatchdog)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::WatchdogReset),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::IntWatchdog)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::Brownout),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::Brownout)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::PowerOn)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::Panic)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultCode::None),
                          static_cast<int>(BootGuard::resetFaultCode(ResetReason::Unknown)));
}

// Robustness: a backwards clock jump must never clear the streak (it means
// "did not survive", and 0 is the safe direction to refuse).
void test_boot_backwards_clock_never_clears(void) {
    BootGuard g;
    g.onBoot(1, ResetReason::Software);
    TEST_ASSERT_FALSE(g.tickSurvived(100, 600000));  // now < boot
    TEST_ASSERT_EQUAL_UINT8(2, g.streak());
}
