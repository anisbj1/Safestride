// bw_diag — QualityHysteresis tests (spec §15.5 item 1; D7 fail-safe
// asymmetry: Insufficient immediate, Degraded after kDegradedEnterWindows,
// Good after kGoodRecoverWindows).

#include <bw/diag/diag_config.h>
#include <bw/diag/quality_hysteresis.h>
#include <unity.h>

using bw::DataQuality;
using bw::diag::QualityHysteresis;

namespace {

constexpr auto kGood = DataQuality::Good;
constexpr auto kDegraded = DataQuality::Degraded;
constexpr auto kInsufficient = DataQuality::Insufficient;

}  // namespace

// §10 fail-safe: boot state is Insufficient; one Good window is not enough
// — a full recovery count is required before the source is trusted.
void test_hysteresis_boot_is_insufficient_fail_safe(void) {
    QualityHysteresis h;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.current()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.update(kGood)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.update(kGood)));
}

// Good -> Degraded exactly at kDegradedEnterWindows (2), not before.
void test_hysteresis_degraded_enters_after_enter_windows(void) {
    QualityHysteresis h;
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kGood);  // boot recovery complete
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.current()));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.update(kDegraded)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded),
                          static_cast<int>(h.update(kDegraded)));  // 2nd window enters
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.current()));
}

// A single bad window is a glitch: no committed change.
void test_hysteresis_single_bad_window_no_change(void) {
    QualityHysteresis h;
    for (int i = 0; i < 3; ++i) {
        (void)h.update(kGood);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.update(kDegraded)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.current()));
    // and the next Good window restarts the run
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.current()));
}

// Recovery Degraded -> Good needs the full kGoodRecoverWindows (3).
void test_hysteresis_good_recovers_after_recover_windows(void) {
    QualityHysteresis h;
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kDegraded);
    (void)h.update(kDegraded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.current()));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.update(kGood)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.update(kGood)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood),
                          static_cast<int>(h.update(kGood)));  // 3rd window recovers
}

// Insufficient never waits: one bad window drops the source (D7).
void test_hysteresis_insufficient_enters_immediately(void) {
    QualityHysteresis h;
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kGood);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient),
                          static_cast<int>(h.update(kInsufficient)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.current()));
}

// Insufficient -> Good pays the full recovery count (§15.5 item 1).
void test_hysteresis_insufficient_to_good_needs_full_recovery(void) {
    QualityHysteresis h;
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kInsufficient);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.update(kGood)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.update(kGood)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.update(kGood)));
}

// Flapping Good/Degraded noise from a Degraded state can never accumulate
// a recovery run: the filter settles at Degraded (§15.5 item 1).
void test_hysteresis_flapping_settles_at_degraded(void) {
    QualityHysteresis h;
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kGood);
    (void)h.update(kDegraded);
    (void)h.update(kDegraded);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.current()));

    for (int i = 0; i < 10; ++i) {
        const DataQuality input = (i % 2 == 0) ? kGood : kDegraded;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(kDegraded), static_cast<int>(h.update(input)));
    }
}

// reset() returns to the fail-safe boot state.
void test_hysteresis_reset_returns_to_fail_safe(void) {
    QualityHysteresis h;
    for (int i = 0; i < 3; ++i) {
        (void)h.update(kGood);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kGood), static_cast<int>(h.current()));
    h.reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kInsufficient), static_cast<int>(h.current()));
}
