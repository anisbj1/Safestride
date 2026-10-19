// F11 §9 — ButtonDebouncer: raw level -> stable level, time-driven.

#include <bw/input/button_debouncer.h>

#include <unity.h>

using bw::input::ButtonDebouncer;
using bw::input::kDebounceMs;

namespace {

// Stable press lands exactly kDebounceMs after the first raw-high sample.
void test_debouncer_clean_press_and_release(void) {
    ButtonDebouncer d;
    TEST_ASSERT_FALSE(d.step(0, bw::ButtonId::Big, false).changed);

    bw::input::ButtonDebouncer::Level l = d.step(0, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(l.changed);
    TEST_ASSERT_FALSE(l.pressed);

    l = d.step(10, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(l.changed);

    l = d.step(20, bw::ButtonId::Big, true);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_TRUE(l.pressed);
    TEST_ASSERT_TRUE(d.pressed(bw::ButtonId::Big));

    l = d.step(100, bw::ButtonId::Big, false);
    TEST_ASSERT_FALSE(l.changed);
    TEST_ASSERT_TRUE(l.pressed);

    l = d.step(120, bw::ButtonId::Big, false);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_FALSE(l.pressed);
}

// Boundary (F11 §9): a level held for exactly kDebounceMs is accepted.
void test_debouncer_accepts_at_exact_window(void) {
    ButtonDebouncer d;
    (void)d.step(5, bw::ButtonId::Small, true);
    (void)d.step(15, bw::ButtonId::Small, true);
    const bw::input::ButtonDebouncer::Level l = d.step(25, bw::ButtonId::Small, true);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_TRUE(l.pressed);
}

// Boundary: one millisecond short of the window is NOT accepted yet.
void test_debouncer_rejects_one_sample_less(void) {
    ButtonDebouncer d;
    (void)d.step(5, bw::ButtonId::Small, true);
    (void)d.step(15, bw::ButtonId::Small, true);
    bw::input::ButtonDebouncer::Level l = d.step(24, bw::ButtonId::Small, true);
    TEST_ASSERT_FALSE(l.changed);
    TEST_ASSERT_FALSE(l.pressed);

    l = d.step(25, bw::ButtonId::Small, true);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_TRUE(l.pressed);
}

// A 10 ms bounce burst (raw flips every tick, worst case inside the window)
// must never reach the stable output; once the level settles, one change.
void test_debouncer_bounce_burst_rejected(void) {
    ButtonDebouncer d;
    bw::TimeMs t = 0;
    bool raw = false;
    uint8_t changes = 0;
    for (uint8_t i = 0; i < 20; ++i) {  // 200 ms of chatter
        raw = !raw;
        if (d.step(t, bw::ButtonId::Big, raw).changed) {
            ++changes;
        }
        t += 10;
    }
    TEST_ASSERT_EQUAL_UINT8(0, changes);

    // Settle high: exactly one change, only after the full window.
    TEST_ASSERT_FALSE(d.step(200, bw::ButtonId::Big, true).changed);
    TEST_ASSERT_FALSE(d.step(210, bw::ButtonId::Big, true).changed);
    TEST_ASSERT_TRUE(d.step(220, bw::ButtonId::Big, true).changed);
    TEST_ASSERT_TRUE(d.pressed(bw::ButtonId::Big));
}

// The two buttons keep independent state.
void test_debouncer_buttons_independent(void) {
    ButtonDebouncer d;
    (void)d.step(0, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(d.pressed(bw::ButtonId::Small));
    (void)d.step(10, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(d.pressed(bw::ButtonId::Small));
    (void)d.step(20, bw::ButtonId::Big, true);
    TEST_ASSERT_TRUE(d.pressed(bw::ButtonId::Big));
    TEST_ASSERT_FALSE(d.pressed(bw::ButtonId::Small));
}

// Pure alternation every tick for 100 ticks: the candidate window restarts
// each flip, so the stable level never moves.
void test_debouncer_chatter_never_toggles(void) {
    ButtonDebouncer d;
    bw::TimeMs t = 0;
    bool raw = false;
    uint8_t changes = 0;
    for (uint8_t i = 0; i < 100; ++i) {
        raw = !raw;
        if (d.step(t, bw::ButtonId::Small, raw).changed) {
            ++changes;
        }
        t += 10;
    }
    TEST_ASSERT_EQUAL_UINT8(0, changes);
}

// reset() drops accumulated state (restart semantics, F11 D6).
void test_debouncer_reset_clears_state(void) {
    ButtonDebouncer d;
    (void)d.step(0, bw::ButtonId::Big, true);
    (void)d.step(10, bw::ButtonId::Big, true);
    (void)d.step(20, bw::ButtonId::Big, true);
    TEST_ASSERT_TRUE(d.pressed(bw::ButtonId::Big));

    d.reset();
    TEST_ASSERT_FALSE(d.pressed(bw::ButtonId::Big));
    TEST_ASSERT_FALSE(d.step(30, bw::ButtonId::Big, true).changed);  // window restarted
}

// A sample stamped earlier than the recorded candidate (clock rewind) restarts
// the window instead of accepting instantly via an unsigned underflow.
void test_debouncer_time_rewind_restarts_window(void) {
    ButtonDebouncer d;
    (void)d.step(1000, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(d.step(1010, bw::ButtonId::Big, true).changed);

    // Rewind: candidate_since moves to 500; the window must re-run.
    bw::input::ButtonDebouncer::Level l = d.step(500, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(l.changed);

    l = d.step(519, bw::ButtonId::Big, true);
    TEST_ASSERT_FALSE(l.changed);

    l = d.step(520, bw::ButtonId::Big, true);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_TRUE(l.pressed);
}

}  // namespace

void register_debouncer_tests(void) {
    RUN_TEST(test_debouncer_clean_press_and_release);
    RUN_TEST(test_debouncer_accepts_at_exact_window);
    RUN_TEST(test_debouncer_rejects_one_sample_less);
    RUN_TEST(test_debouncer_bounce_burst_rejected);
    RUN_TEST(test_debouncer_buttons_independent);
    RUN_TEST(test_debouncer_chatter_never_toggles);
    RUN_TEST(test_debouncer_reset_clears_state);
    RUN_TEST(test_debouncer_time_rewind_restarts_window);
}
