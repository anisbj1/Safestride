// bw_diag — task registry helper tests (spec §16.4 item 3): worstLatenessMs /
// worstStackFreeBytes / slotBelowStackFloor over healthy / empty / all-invalid
// arrays, plus the first-call empty state and the 0 = not-yet-measured rule
// (bw/task_health.h; unknown ≠ 0 — D13/D14 honesty rules).

#include <bw/diag/task_registry.h>
#include <unity.h>

#include "diag_test_support.h"

using bw::diag::slotBelowStackFloor;
using bw::diag::TaskSlot;
using bw::diag::worstLatenessMs;
using bw::diag::worstStackFreeBytes;
using bw_diag_test::notedTask;

// Healthy array: max lateness, min measured watermark, floor decision.
void test_task_registry_healthy_array(void) {
    const bw::TaskHealth slots[] = {
        notedTask(10, 700),
        notedTask(40, 511),
        notedTask(25, 900),
    };
    TEST_ASSERT_EQUAL_UINT16(40, worstLatenessMs(slots, 3));
    TEST_ASSERT_EQUAL_UINT16(511, worstStackFreeBytes(slots, 3));
    // Strict "<" floor (fault.h): 511 < 512 ⇒ below, 512 ⇒ not.
    TEST_ASSERT_TRUE(slotBelowStackFloor(slots, 3, 512));
    TEST_ASSERT_FALSE(slotBelowStackFloor(slots, 3, 511));
}

// Invalid slots are EXCLUDED, never counted as 0: an invalid slot holding
// garbage 200 ms lateness and 1 free byte must contribute nothing.
void test_task_registry_invalid_slots_excluded(void) {
    bw::TaskHealth slots[3]{};
    slots[0] = notedTask(5, 800);
    slots[1].worst_lateness_ms = 200;  // valid=false (never noted): garbage
    slots[1].stack_free_min_bytes = 1;
    slots[2] = notedTask(0, 700);

    TEST_ASSERT_EQUAL_UINT16(5, worstLatenessMs(slots, 3));  // not 200, not lower
    TEST_ASSERT_EQUAL_UINT16(700, worstStackFreeBytes(slots, 3));
    TEST_ASSERT_FALSE(slotBelowStackFloor(slots, 3, 512));
}

// All-invalid (every task still unadopted at P4): no evidence at all —
// 0 / 0 / false, so the monitor diagnoses nothing (unknown ≠ 0-free).
void test_task_registry_all_invalid_no_evidence(void) {
    const bw::TaskHealth slots[4]{};  // default: valid=false everywhere
    TEST_ASSERT_EQUAL_UINT16(0, worstLatenessMs(slots, 4));
    TEST_ASSERT_EQUAL_UINT16(0, worstStackFreeBytes(slots, 4));
    TEST_ASSERT_FALSE(slotBelowStackFloor(slots, 4, 512));
}

// First-call / empty state: nullptr with count 0 behaves exactly like the
// all-invalid array (the honest "no registry yet" shape, DiagInput
// default tasks=nullptr task_count=0).
void test_task_registry_empty_and_first_call(void) {
    TEST_ASSERT_EQUAL_UINT16(0, worstLatenessMs(nullptr, 0));
    TEST_ASSERT_EQUAL_UINT16(0, worstStackFreeBytes(nullptr, 0));
    TEST_ASSERT_FALSE(slotBelowStackFloor(nullptr, 0, 512));

    // A valid slot that never measured its watermark (0 = "not yet
    // measured", bw/task_health.h) is unknown — it must NOT read as
    // 0 free bytes below the floor.
    bw::TaskHealth unmeasured{};
    unmeasured.noteCycle(1000, 0, 0);
    TEST_ASSERT_TRUE(unmeasured.valid);
    TEST_ASSERT_EQUAL_UINT16(0, worstStackFreeBytes(&unmeasured, 1));
    TEST_ASSERT_FALSE(slotBelowStackFloor(&unmeasured, 1, 512));
    // ...while its (real) lateness still counts as evidence.
    TEST_ASSERT_EQUAL_UINT16(0, worstLatenessMs(&unmeasured, 1));
}

// The Slot enum is a registry INDEX: Count == kMaxTrackedTasks and
// DiagSelf == kDiagSelfTaskSlot are pinned by static_assert in the
// header; this pins the runtime-visible values too (§8 M5).
void test_task_registry_slot_values(void) {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(TaskSlot::Sensor));
    TEST_ASSERT_EQUAL_UINT8(11, static_cast<uint8_t>(TaskSlot::DiagSelf));
    TEST_ASSERT_EQUAL_UINT8(bw::diag::kMaxTrackedTasks, static_cast<uint8_t>(TaskSlot::Count));
}
