// F00 §9: TaskHealth — validity flag, max-keeping lateness, stack reported as measured.

#include <bw/task_health.h>

#include <unity.h>

void test_task_health_invalid_before_first_cycle(void) {
    bw::TaskHealth h{};
    // "Never ran" is an ordinary state, not a magic value (02 §1)
    TEST_ASSERT_FALSE(h.valid);
    TEST_ASSERT_EQUAL_UINT64(0, h.last_run_ms);
    TEST_ASSERT_EQUAL_UINT16(0, h.worst_lateness_ms);
    TEST_ASSERT_EQUAL_UINT16(0, h.stack_free_min_bytes);  // 0 = not yet measured
}

void test_task_health_first_cycle_marks_valid(void) {
    bw::TaskHealth h;  // no braces: the default initializers must still start invalid
    TEST_ASSERT_FALSE(h.valid);

    h.noteCycle(1000, 12, 2048);
    TEST_ASSERT_TRUE(h.valid);
    TEST_ASSERT_EQUAL_UINT64(1000, h.last_run_ms);
    TEST_ASSERT_EQUAL_UINT16(12, h.worst_lateness_ms);
    TEST_ASSERT_EQUAL_UINT16(2048, h.stack_free_min_bytes);
}

void test_task_health_worst_lateness_keeps_max_never_descends(void) {
    bw::TaskHealth h;
    h.noteCycle(1000, 50, 0);
    h.noteCycle(2000, 10, 0);  // a better cycle must not lower the running worst
    TEST_ASSERT_EQUAL_UINT16(50, h.worst_lateness_ms);

    h.noteCycle(3000, 120, 0);  // a new worst replaces it
    TEST_ASSERT_EQUAL_UINT16(120, h.worst_lateness_ms);

    h.noteCycle(4000, 3, 0);
    TEST_ASSERT_EQUAL_UINT16(120, h.worst_lateness_ms);
    // last_run keeps advancing even while the worst stays put
    TEST_ASSERT_EQUAL_UINT64(4000, h.last_run_ms);
    TEST_ASSERT_TRUE(h.valid);
}

void test_task_health_lateness_saturates_not_wraps(void) {
    bw::TaskHealth h;
    h.noteCycle(1000, 70000u, 0);  // > UINT16_MAX: must clamp, not wrap to a small number
    TEST_ASSERT_EQUAL_UINT16(65535, h.worst_lateness_ms);
}

void test_task_health_stack_reported_as_measured(void) {
    bw::TaskHealth h;
    h.noteCycle(1000, 0, 4096);
    TEST_ASSERT_EQUAL_UINT16(4096, h.stack_free_min_bytes);

    // Stored exactly as reported — the device-side high-water mark is already the
    // running minimum (FreeRTOS), so the struct must not second-guess it.
    h.noteCycle(2000, 0, 3584);
    TEST_ASSERT_EQUAL_UINT16(3584, h.stack_free_min_bytes);
}
