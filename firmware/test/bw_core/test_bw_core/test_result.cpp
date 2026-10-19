#include <bw/result.h>

#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

// Every Err enumerator, in declaration order (02 §2). Append-only: adding a code at
// the end is legal, renumbering is not — that is exactly what the pin below guards.
struct ErrTableRow {
    bw::Err err;
    uint8_t numeric;
    const char* name;
};

constexpr ErrTableRow kErrTable[] = {
    {bw::Err::Ok, 0, "Ok"},
    {bw::Err::Timeout, 1, "Timeout"},
    {bw::Err::BusError, 2, "BusError"},
    {bw::Err::NotFound, 3, "NotFound"},
    {bw::Err::InvalidArg, 4, "InvalidArg"},
    {bw::Err::NotReady, 5, "NotReady"},
    {bw::Err::Overflow, 6, "Overflow"},
    {bw::Err::Corrupt, 7, "Corrupt"},
    {bw::Err::Unsupported, 8, "Unsupported"},
    {bw::Err::Busy, 9, "Busy"},
    {bw::Err::HwFault, 10, "HwFault"},
    {bw::Err::Full, 11, "Full"},
};

// D5's own guarantee: Result travels through queues/mailboxes like any contract
// struct (02 §1), so it must stay trivially copyable.
static_assert(std::is_trivially_copyable_v<bw::Result<int>>,
              "Result<int> must be trivially copyable (02 §1)");
static_assert(std::is_trivially_copyable_v<bw::Result<void>>,
              "Result<void> must be trivially copyable (02 §1)");
static_assert(std::is_trivially_copyable_v<bw::Result<bw::TimeMs>>,
              "Result<TimeMs> must be trivially copyable (02 §1)");

}  // namespace

void test_err_values_pinned_in_declaration_order(void) {
    for (const ErrTableRow& row : kErrTable) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Err::%s must stay %u (append-only, 02 §6.6)", row.name,
                 static_cast<unsigned>(row.numeric));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row.numeric, static_cast<uint8_t>(row.err), msg);
    }
    // CR-004's capacity family must keep both members: Full (refused) and Overflow
    // (already lost) are deliberately distinct codes.
    TEST_ASSERT_NOT_EQUAL(static_cast<uint8_t>(bw::Err::Full),
                          static_cast<uint8_t>(bw::Err::Overflow));
}

void test_result_ok_true_only_for_err_ok(void) {
    for (const ErrTableRow& row : kErrTable) {
        bw::Result<int> r{row.err, 7};
        char msg[96];
        snprintf(msg, sizeof(msg), "Result with Err::%s", row.name);
        if (row.err == bw::Err::Ok) {
            TEST_ASSERT_TRUE_MESSAGE(r.ok(), msg);
        } else {
            TEST_ASSERT_FALSE_MESSAGE(r.ok(), msg);
        }
    }
}

void test_result_holds_value_on_success(void) {
    bw::Result<int> r{bw::Err::Ok, 42};
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(42, r.value);
}

void test_result_value_default_constructed_when_err_only(void) {
    // `value` is value-initialized to a known default when not supplied — a default
    // object, documented as meaningless unless ok() (F00 D5). The explicit `{}` keeps
    // -Wextra's missing-field-initializers quiet.
    bw::Result<int> r{bw::Err::NotFound, {}};
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(0, r.value);
}

void test_result_stores_value_even_on_error_path(void) {
    // The stored value exists but is undefined for the caller unless ok() — this test
    // documents that reading it is a caller bug, not that it is zeroed.
    bw::Result<int> r{bw::Err::Timeout, 42};
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(42, r.value);
}

void test_result_void_specialization(void) {
    bw::Result<void> ok{bw::Err::Ok};
    TEST_ASSERT_TRUE(ok.ok());

    bw::Result<void> bad{bw::Err::Busy};
    TEST_ASSERT_FALSE(bad.ok());

    static_assert(sizeof(bw::Result<void>) == sizeof(bw::Err),
                  "Result<void> carries nothing but the error code (02 §2)");
}
