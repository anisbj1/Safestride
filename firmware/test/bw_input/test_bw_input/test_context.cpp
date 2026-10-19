// F11 §9 — InputContext derivation (D4) and the bw::ButtonEvent conversion
// (F11 §3 outputs; the chord is deliberately not representable).

#include <bw/input/input_types.h>

#include <unity.h>

namespace {

using bw::input::deriveContext;
using bw::input::GestureKey;
using bw::input::toButtonEvent;

bw::input::InputContextId deriveAt(bw::AlertLevel level) {
    bw::AlertStatus status{};
    status.t_ms = 1000;
    status.level = level;
    status.acknowledged = false;
    status.reasons = 0;
    status.since_ms = 0;
    return deriveContext(&status);
}

// Empty mailbox (no AlertStatus published yet) is Idle — a tested state, never
// a guess (F11 D4).
void test_context_null_alert_is_idle(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Idle),
                          static_cast<int>(deriveContext(nullptr)));
}

void test_context_normal_level_is_idle(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Idle),
                          static_cast<int>(deriveAt(bw::AlertLevel::Normal)));
}

void test_context_warning_level_is_alert(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::AlertActive),
                          static_cast<int>(deriveAt(bw::AlertLevel::Warning)));
}

void test_context_active_level_is_alert(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::AlertActive),
                          static_cast<int>(deriveAt(bw::AlertLevel::Active)));
}

void test_context_persistent_level_is_alert(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::AlertActive),
                          static_cast<int>(deriveAt(bw::AlertLevel::Persistent)));
}

// InputContext is a value type: whatever id is set passes through unchanged
// (ConfirmPrompt/Menu have no AlertStatus source in wave 1, D4).
void test_context_value_passes_through(void) {
    bw::input::InputContext ctx{bw::input::InputContextId::ConfirmPrompt};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::ConfirmPrompt),
                          static_cast<int>(ctx.id));
    ctx.id = bw::input::InputContextId::Menu;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Menu),
                          static_cast<int>(ctx.id));
}

// --- Menu closure (backlog 32): the F12 menu_open seam ----------------------
// The single-argument overload must stay exactly the wave-1 function so no
// existing caller changes meaning (menu_open == false behind the scenes).
void test_context_menu_flag_false_matches_single_arg(void) {
    bw::AlertStatus status{};
    status.level = bw::AlertLevel::Normal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Idle),
                          static_cast<int>(deriveContext(&status, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Idle),
                          static_cast<int>(deriveContext(nullptr, false)));
}

// menu_open is the ONLY new source: with a quiet alert side it yields Menu.
void test_context_menu_open_is_menu(void) {
    bw::AlertStatus status{};
    status.level = bw::AlertLevel::Normal;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Menu),
                          static_cast<int>(deriveContext(&status, true)));
}

// No AlertStatus published yet + menu open is still Menu (an empty mailbox is
// not an alert; D4 says empty -> Idle, and the menu overrides that only).
void test_context_menu_open_with_null_alert_is_menu(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::Menu),
                          static_cast<int>(deriveContext(nullptr, true)));
}

// AlertActive outranks Menu: a raised alert is never blocked by UI state, so
// every raised level returns AlertActive even while the menu is open.
void test_context_alert_outranks_menu(void) {
    const bw::AlertLevel levels[] = {bw::AlertLevel::Warning, bw::AlertLevel::Active,
                                     bw::AlertLevel::Persistent};
    for (const bw::AlertLevel level : levels) {
        bw::AlertStatus status{};
        status.level = level;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::InputContextId::AlertActive),
                              static_cast<int>(deriveContext(&status, true)));
    }
}

// Single-button gestures map 1:1 onto bw::ButtonEvent (§3).
void test_to_button_event_single_gestures(void) {
    bw::ButtonEvent ev{};
    const bw::input::GestureRecord big_short{111, GestureKey::BigShort};
    TEST_ASSERT_TRUE(toButtonEvent(big_short, ev));
    TEST_ASSERT_EQUAL_UINT64(111, ev.t_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonId::Big), static_cast<int>(ev.button));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::ShortPress),
                          static_cast<int>(ev.gesture));

    const bw::input::GestureRecord big_long{222, GestureKey::BigLong};
    TEST_ASSERT_TRUE(toButtonEvent(big_long, ev));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonId::Big), static_cast<int>(ev.button));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::LongPress),
                          static_cast<int>(ev.gesture));

    const bw::input::GestureRecord small_short{333, GestureKey::SmallShort};
    TEST_ASSERT_TRUE(toButtonEvent(small_short, ev));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonId::Small), static_cast<int>(ev.button));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::ShortPress),
                          static_cast<int>(ev.gesture));

    const bw::input::GestureRecord small_long{444, GestureKey::SmallLong};
    TEST_ASSERT_TRUE(toButtonEvent(small_long, ev));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonId::Small), static_cast<int>(ev.button));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::LongPress),
                          static_cast<int>(ev.gesture));
}

// The chord has no bw::ButtonGesture value (documented contract limitation):
// conversion reports false instead of inventing one.
void test_to_button_event_chord_not_representable(void) {
    bw::ButtonEvent ev{};
    ev.gesture = bw::ButtonGesture::ShortPress;  // marker
    const bw::input::GestureRecord chord{555, GestureKey::Chord};
    TEST_ASSERT_FALSE(toButtonEvent(chord, ev));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::ShortPress),
                          static_cast<int>(ev.gesture));  // untouched
}

}  // namespace

// Registration entry point called by main.cpp.
void register_context_tests(void) {
    RUN_TEST(test_context_null_alert_is_idle);
    RUN_TEST(test_context_normal_level_is_idle);
    RUN_TEST(test_context_warning_level_is_alert);
    RUN_TEST(test_context_active_level_is_alert);
    RUN_TEST(test_context_persistent_level_is_alert);
    RUN_TEST(test_context_value_passes_through);
    RUN_TEST(test_context_menu_flag_false_matches_single_arg);
    RUN_TEST(test_context_menu_open_is_menu);
    RUN_TEST(test_context_menu_open_with_null_alert_is_menu);
    RUN_TEST(test_context_alert_outranks_menu);
    RUN_TEST(test_to_button_event_single_gestures);
    RUN_TEST(test_to_button_event_chord_not_representable);
}
