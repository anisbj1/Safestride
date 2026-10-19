#include <bw/input/input_engine.h>
#include <initializer_list>
#include <unity.h>

namespace {
using namespace bw;
using namespace bw::input;

void test_alert_press_is_owned_through_release_into_menu_or_idle() {
    for (const auto after : {InputContextId::Menu, InputContextId::Idle}) {
        for (bool long_press : {false, true}) {
            InputEngine engine;
            (void)engine.step(0, {}, false, false);
            (void)engine.step(10, {InputContextId::AlertActive}, true, false);
            (void)engine.step(30, {InputContextId::AlertActive}, true, false);
            const TimeMs release = long_press ? 1000 : 200;
            (void)engine.step(release, {after}, false, false);
            const auto out = engine.step(release + kDebounceMs, {after}, false, false);
            TEST_ASSERT_EQUAL_UINT8(1, out.count);
            TEST_ASSERT_TRUE(out.items[0].alert_consumed);
            TEST_ASSERT_TRUE(out.items[0].has_command);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(long_press ? CommandType::CancelAlert
                                                              : CommandType::AcknowledgeAlert),
                                  static_cast<int>(out.items[0].command.type));
            // The very next independent press is normal again.
            (void)engine.step(release + 100, {}, true, false);
            (void)engine.step(release + 120, {}, true, false);
            (void)engine.step(release + 300, {}, false, false);
            const auto next = engine.step(release + 320, {}, false, false);
            TEST_ASSERT_FALSE(next.items[0].alert_consumed);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandType::OpenMenu),
                                  static_cast<int>(next.items[0].command.type));
        }
    }
}

void test_alert_overlaps_menu_press_before_release_consumes_navigation() {
    for (bool big : {false, true}) {
        for (bool long_press : {false, true}) {
            InputEngine engine;
            (void)engine.step(0, {InputContextId::Menu}, false, false);
            (void)engine.step(10, {InputContextId::Menu}, big, !big);
            (void)engine.step(30, {InputContextId::Menu}, big, !big);
            (void)engine.step(40, {InputContextId::AlertActive}, big, !big);
            const TimeMs release = long_press ? 1000 : 200;
            (void)engine.step(release, {InputContextId::Menu}, false, false);
            const auto out =
                engine.step(release + kDebounceMs, {InputContextId::Menu}, false, false);
            TEST_ASSERT_EQUAL_UINT8(1, out.count);
            TEST_ASSERT_TRUE(out.items[0].alert_consumed);
            TEST_ASSERT_EQUAL(big, out.items[0].has_command);
        }
    }
}

void test_alert_chord_cannot_pair_or_leak_partner_release() {
    InputEngine engine;
    (void)engine.step(0, {}, false, false);
    (void)engine.step(10, {InputContextId::AlertActive}, true, true);
    (void)engine.step(30, {InputContextId::AlertActive}, true, true);
    (void)engine.step(4000, {InputContextId::Menu}, false, true);
    TEST_ASSERT_EQUAL_UINT8(0, engine.step(4020, {InputContextId::Menu}, false, true).count);
    (void)engine.step(4100, {InputContextId::Menu}, false, false);
    const auto out = engine.step(4120, {InputContextId::Menu}, false, false);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    TEST_ASSERT_TRUE(out.items[0].alert_consumed);
    TEST_ASSERT_FALSE(out.items[0].has_command);
    TEST_ASSERT_EQUAL_UINT8(0, engine.step(4200, {InputContextId::Menu}, false, false).count);
}
}  // namespace

void register_alert_consumption_tests() {
    RUN_TEST(test_alert_press_is_owned_through_release_into_menu_or_idle);
    RUN_TEST(test_alert_overlaps_menu_press_before_release_consumes_navigation);
    RUN_TEST(test_alert_chord_cannot_pair_or_leak_partner_release);
}
