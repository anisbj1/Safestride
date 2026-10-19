// F11 §9 — GestureDetector: release-based emission (D3), chord, boot-ignore,
// the 10 s stuck rule (D5). Levels here are already debounced, so boundaries
// are exact: the script feeds stable levels straight into the detector.

#include <bw/input/gesture_detector.h>

#include <unity.h>

namespace {

using bw::input::GestureDetector;
using bw::input::GestureKey;

// Mirrors the engine's call pattern: BOTH buttons are sampled every tick
// (changed or not), so the chord rule always sees fresh partner levels.
struct DetScript {
    GestureDetector det{};
    bw::TimeMs t = 0;
    bool big = false;
    bool small = false;
    bool prev[2] = {false, false};

    bw::input::GestureRecord rec[8]{};
    uint8_t rec_count = 0;
    uint8_t stuck_edges = 0;

    void tick(uint32_t n = 1, bw::TimeMs dt = 10) {
        for (uint32_t i = 0; i < n; ++i) {
            feed(bw::ButtonId::Big, big);
            feed(bw::ButtonId::Small, small);
            t += dt;
        }
    }

    void feed(bw::ButtonId b, bool pressed) {
        const uint8_t idx = bw::input::buttonIndex(b);
        const bool changed = pressed != prev[idx];
        prev[idx] = pressed;
        const GestureDetector::Output out = det.step(t, b, pressed, changed);
        if (out.has_event && rec_count < 8) {
            rec[rec_count++] = out.event;
        }
        if (out.stuck_edge) {
            ++stuck_edges;
        }
    }
};

void test_gesture_short_emitted_on_release(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(30);  // 300 ms held
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[0].key));
    TEST_ASSERT_EQUAL_UINT64(s.t - 10, s.rec[0].t_ms);  // released on the previous tick
}

// Exact long-press boundary: release AT kLongPressMs -> LongPress.
void test_gesture_long_at_exact_threshold(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);  // press edge lands on this tick
    const bw::TimeMs press_at = s.t - 10;
    s.tick(79);
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);  // nothing while held (D3)
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigLong), static_cast<int>(s.rec[0].key));
    TEST_ASSERT_EQUAL_UINT64(press_at + 800, s.rec[0].t_ms);
}

// One millisecond short of the threshold is a short press (§9 boundaries).
void test_gesture_long_boundary_799(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1, 1);    // press edge at 1 ms resolution
    s.tick(798, 1);  // release edge at press + 799 ms
    s.big = false;
    s.tick(1, 1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[0].key));
}

// One millisecond over: a long press.
void test_gesture_long_boundary_801(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1, 1);
    s.tick(800, 1);  // release edge at press + 801 ms
    s.big = false;
    s.tick(1, 1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigLong), static_cast<int>(s.rec[0].key));
}

// D3: a five-second hold stays silent until release, then reports LongPress.
void test_gesture_no_event_while_held(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(500);  // 5 s
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigLong), static_cast<int>(s.rec[0].key));
}

// Chord: exactly one event, on the later release (D3), when both were down
// together for >= kChordHoldMs.
void test_gesture_chord_emits_once_on_later_release(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);  // Big press edge at t0
    s.small = true;
    s.tick(1);    // Small press edge at t0+10 -> chord window starts
    s.tick(300);  // both down for > 3000 ms together
    s.big = false;
    s.tick(1);  // first release: silent decision
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);
    s.small = false;
    s.tick(1);  // later release: the chord
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::Chord), static_cast<int>(s.rec[0].key));
    s.tick(10);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);  // exactly once
}

// Chord aborted below kChordHoldMs: no chord AND no stray singles (absorbed).
void test_gesture_chord_aborted_below_hold(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);
    s.small = true;
    s.tick(1);
    s.tick(298);  // both down 2990 ms < 3000 ms
    s.big = false;
    s.tick(1);
    s.small = false;
    s.tick(10);
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);
}

// After an aborted attempt both buttons work normally again.
void test_gesture_chord_abort_recovers(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);
    s.small = true;
    s.tick(1);
    s.tick(298);
    s.big = false;
    s.tick(1);
    s.small = false;
    s.tick(2);

    s.small = true;  // a fresh single press after the aborted attempt
    s.tick(30);
    s.small = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::SmallShort), static_cast<int>(s.rec[0].key));
}

// kChordReleaseMs: a re-press inside the window after a chord is silent.
void test_gesture_chord_release_window_suppresses_repress(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);
    s.small = true;
    s.tick(1);
    s.tick(300);
    s.big = false;
    s.tick(1);
    s.small = false;
    s.tick(1);  // chord emitted here
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);

    s.big = true;  // inside the release window
    s.tick(10);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);  // suppressed

    s.tick(60);  // past the window
    s.big = true;
    s.tick(30);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(2, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[1].key));
}

// Boundary: a press that STARTS exactly at suppress_until_ is not suppressed.
void test_gesture_chord_window_boundary_not_suppressed(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);
    s.small = true;
    s.tick(1);
    s.tick(300);
    s.big = false;
    s.tick(1);
    s.small = false;
    s.tick(1);  // chord lands on this tick
    const bw::TimeMs chord_at = s.t - 10;

    // Advance so the next feed sits exactly at chord_at + kChordReleaseMs.
    while (s.t < chord_at + bw::input::kChordReleaseMs) {
        s.tick(1);
    }
    TEST_ASSERT_EQUAL_UINT64(chord_at + bw::input::kChordReleaseMs, s.t);
    s.big = true;
    s.tick(1);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(2, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[1].key));
}

// §9: a button held from the very first sample never becomes a gesture.
void test_gesture_boot_held_ignored_until_release(void) {
    DetScript s;
    s.big = true;  // held at the first observation
    s.tick(50);    // 500 ms
    s.big = false;
    s.tick(2);
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);

    s.big = true;  // a fresh press afterwards works
    s.tick(30);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[0].key));
}

// §9 + D5: a boot-held button still crosses kButtonStuckMs (hardware truth),
// reports exactly once, and its release stays silent.
void test_gesture_boot_held_reports_stuck_once(void) {
    DetScript s;
    s.big = true;
    s.tick(999);  // 9990 ms — below the threshold
    TEST_ASSERT_EQUAL_UINT8(0, s.stuck_edges);
    s.tick(2);  // crosses 10 s
    TEST_ASSERT_EQUAL_UINT8(1, s.stuck_edges);
    s.tick(500);
    TEST_ASSERT_EQUAL_UINT8(1, s.stuck_edges);  // never repeats
    s.big = false;
    s.tick(2);
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);  // release of a stuck button: silent
}

// D5 end to end: stuck suppresses the 11 s "long press" that would otherwise
// escape on release, and the button re-arms after release.
void test_gesture_stuck_suppresses_and_rearms(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);     // press edge
    s.tick(1100);  // 11 s held
    TEST_ASSERT_EQUAL_UINT8(1, s.stuck_edges);
    s.big = false;
    s.tick(2);
    TEST_ASSERT_EQUAL_UINT8(0, s.rec_count);  // nothing escapes

    s.big = true;  // fresh press: gestures work again
    s.tick(30);
    s.big = false;
    s.tick(1);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigShort), static_cast<int>(s.rec[0].key));
}

// A second stuck episode reports a second edge.
void test_gesture_stuck_rearms_for_next_episode(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);
    s.tick(1100);
    s.big = false;
    s.tick(2);
    s.big = true;
    s.tick(1);  // new press edge
    s.tick(1100);
    TEST_ASSERT_EQUAL_UINT8(2, s.stuck_edges);
    s.big = false;
    s.tick(2);
}

// A chord must never start against a boot-ignored partner: the live button
// still gets its own single gesture.
void test_gesture_no_chord_against_boot_held_button(void) {
    DetScript s;
    s.big = true;  // boot-held, ignored
    s.tick(3);
    s.small = true;
    s.tick(30);
    s.small = false;
    s.tick(1);
    s.tick(100);
    s.big = false;
    s.tick(2);
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::SmallShort), static_cast<int>(s.rec[0].key));
}

// A huge gap between samples: durations come from timestamps, so the late
// release observation still reports LongPress (§9 "gaps").
void test_gesture_time_gap_marks_long(void) {
    DetScript s;
    s.tick(3);
    s.big = true;
    s.tick(1);        // press edge
    s.tick(1, 5000);  // observation gap while held
    s.big = false;
    s.tick(1);  // release seen at press + 5 s
    TEST_ASSERT_EQUAL_UINT8(1, s.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GestureKey::BigLong), static_cast<int>(s.rec[0].key));
}

}  // namespace

// Registration entry point called by main.cpp (one RUN_TEST per §9 case).
void register_gesture_detector_tests(void) {
    RUN_TEST(test_gesture_short_emitted_on_release);
    RUN_TEST(test_gesture_long_at_exact_threshold);
    RUN_TEST(test_gesture_long_boundary_799);
    RUN_TEST(test_gesture_long_boundary_801);
    RUN_TEST(test_gesture_no_event_while_held);
    RUN_TEST(test_gesture_chord_emits_once_on_later_release);
    RUN_TEST(test_gesture_chord_aborted_below_hold);
    RUN_TEST(test_gesture_chord_abort_recovers);
    RUN_TEST(test_gesture_chord_release_window_suppresses_repress);
    RUN_TEST(test_gesture_chord_window_boundary_not_suppressed);
    RUN_TEST(test_gesture_boot_held_ignored_until_release);
    RUN_TEST(test_gesture_boot_held_reports_stuck_once);
    RUN_TEST(test_gesture_stuck_suppresses_and_rearms);
    RUN_TEST(test_gesture_stuck_rearms_for_next_episode);
    RUN_TEST(test_gesture_no_chord_against_boot_held_button);
    RUN_TEST(test_gesture_time_gap_marks_long);
}
