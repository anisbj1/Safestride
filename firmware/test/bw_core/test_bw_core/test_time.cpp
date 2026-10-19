#include <bw/time.h>

#include <unity.h>

#include <cstdint>

// All helpers must be usable at compile time (constexpr) as well as at runtime.
static_assert(bw::elapsedMs(10, 4) == 6, "elapsedMs basic case");
static_assert(bw::elapsedMs(4, 4) == 0, "elapsedMs(now == since) == 0");
static_assert(bw::elapsedMs(4, 10) == 0, "elapsedMs saturates at 0, never underflows");
static_assert(!bw::withinMs(4, 10, 100), "withinMs false for a stale timestamp");
static_assert(bw::withinMs(4, 4, 0), "withinMs true at exactly zero window");

void test_elapsed_no_32bit_truncation_across_2pow32(void) {
    // 01 §4 / 03 §4: durations must survive operands that straddle 2^32; a uint32_t
    // intermediate would fold both operands modulo 2^32 and lose the difference.
    const bw::TimeMs since = 0x00000000FFFFFFF0ull;
    const bw::TimeMs now = 0x0000000100000100ull;  // crosses 2^32
    TEST_ASSERT_EQUAL_UINT64(0x110, bw::elapsedMs(now, since));
}

void test_elapsed_zero_when_equal(void) {
    TEST_ASSERT_EQUAL_UINT64(0, bw::elapsedMs(123456, 123456));
}

void test_elapsed_zero_when_now_before_since(void) {
    // Stale or pre-reset timestamp: plain subtraction would return ~1.8e19 ms.
    const bw::TimeMs stale = 5'000'000;
    const bw::TimeMs now = 1'000'000;
    TEST_ASSERT_EQUAL_UINT64(0, bw::elapsedMs(now, stale));
    TEST_ASSERT_FALSE(bw::withinMs(now, stale, 10'000));
}

void test_elapsed_huge_forward_jump(void) {
    const bw::TimeMs since = 1'000'000;
    const bw::TimeMs now = since + 5'000'000'000'000ull;  // 5e12 ms > 2^32
    TEST_ASSERT_EQUAL_UINT64(5'000'000'000'000ull, bw::elapsedMs(now, since));
    TEST_ASSERT_TRUE(bw::withinMs(now, since, 5'000'000'000'000ull));
    TEST_ASSERT_FALSE(bw::withinMs(now, since, 4'999'999'999'999ull));
}

void test_elapsed_near_uint64_max_does_not_wrap(void) {
    const bw::TimeMs now = UINT64_MAX;
    TEST_ASSERT_EQUAL_UINT64(100, bw::elapsedMs(now, now - 100));
    TEST_ASSERT_EQUAL_UINT64(0, bw::elapsedMs(now - 100, now));
    TEST_ASSERT_TRUE(bw::withinMs(now, now - 100, 100));
}

void test_within_boundary_is_inclusive(void) {
    // Within means "at most window ms in the past": exactly window counts, one more
    // does not. The boundary convention is pinned so features cannot drift.
    const bw::TimeMs since = 1'000;
    TEST_ASSERT_TRUE(bw::withinMs(since + 100, since, 100));
    TEST_ASSERT_FALSE(bw::withinMs(since + 101, since, 100));
    TEST_ASSERT_FALSE(bw::withinMs(since, since + 1, 0));
}
