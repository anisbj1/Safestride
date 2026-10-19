// F11 §9 — contract-shaped pins: the constants the spec names, the enum
// values this feature depends on, and the layout of F11's own value types.

#include <bw/fault.h>
#include <bw/input/input_constants.h>
#include <bw/input/input_engine.h>
#include <bw/input/input_router.h>
#include <bw/input/input_types.h>

#include <type_traits>

#include <unity.h>

namespace {

// The spec §8 table and this file must list the same values (H9 drift).
void test_contract_constants_match_spec(void) {
    TEST_ASSERT_EQUAL_UINT64(20, bw::input::kDebounceMs);
    TEST_ASSERT_EQUAL_UINT64(800, bw::input::kLongPressMs);
    TEST_ASSERT_EQUAL_UINT64(3000, bw::input::kChordHoldMs);
    TEST_ASSERT_EQUAL_UINT64(10000, bw::input::kButtonStuckMs);
    TEST_ASSERT_EQUAL_UINT64(500, bw::input::kChordReleaseMs);
    TEST_ASSERT_EQUAL_UINT32(1, bw::input::kStepOnce);
    TEST_ASSERT_EQUAL_UINT8(0, bw::input::kCommandSourceButton);
    TEST_ASSERT_EQUAL_UINT8(4, bw::input::kInputContextCount);
    TEST_ASSERT_EQUAL_UINT8(5, bw::input::kGestureKeyCount);
    TEST_ASSERT_EQUAL_UINT8(2, bw::input::kMaxGesturesPerStep);
}

// FaultCode::ButtonStuck (= 10) is pre-allocated to F11's >10 s path (§10);
// bw_core pins the enum too — this asserts OUR dependency on that value.
void test_contract_button_stuck_code_pinned(void) {
    TEST_ASSERT_EQUAL_UINT8(10, static_cast<uint8_t>(bw::FaultCode::ButtonStuck));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(bw::FaultSeverity::Warning));
}

// ButtonId ordering feeds buttonIndex(); F11 relies on Big=0, Small=1.
void test_contract_button_id_order(void) {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(bw::ButtonId::Big));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(bw::ButtonId::Small));
    TEST_ASSERT_EQUAL_UINT8(0, bw::input::buttonIndex(bw::ButtonId::Big));
    TEST_ASSERT_EQUAL_UINT8(1, bw::input::buttonIndex(bw::ButtonId::Small));
}

// Layout expectations of F11's own value types (queue/mailbox safe).
static_assert(std::is_trivially_copyable_v<bw::input::GestureRecord>,
              "GestureRecord goes through bw_core containers (02 §1)");
static_assert(std::is_trivially_copyable_v<bw::input::InputContext>,
              "InputContext is passed by value (02 §1)");
static_assert(std::is_trivially_copyable_v<bw::input::MapCell>, "MapCell is a plain table row");
static_assert(bw::input::kInputMap.size() == 20, "D2-N table is 4 contexts x 5 gestures");

void test_contract_layout_static_asserts_hold(void) {
    // The static_asserts above are the assertions; this keeps the test list
    // explicit so a missing RUN_TEST would be visible in review.
    TEST_ASSERT_EQUAL_UINT(20, bw::input::kInputMap.size());
}

}  // namespace

// Registration entry point called by main.cpp.
void register_contract_tests(void) {
    RUN_TEST(test_contract_constants_match_spec);
    RUN_TEST(test_contract_button_stuck_code_pinned);
    RUN_TEST(test_contract_button_id_order);
    RUN_TEST(test_contract_layout_static_asserts_hold);
}
