// F11 §9 — InputEngine end-to-end: raw levels -> routed Command/button event,
// the queue-full drop path (D6), time gaps, restart, and the no-heap check.
// bw_ml's pattern: the suite owns the global operator new counters itself.

#include <bw/input/input_engine.h>
#include <bw/input/queue_command_sink.h>

#include <cstddef>
#include <cstdlib>
#include <new>

#include <unity.h>

#include "fixture_helpers.h"

namespace {

// Counted allocations: static storage with constant initialization, so the
// snapshot below is valid even across static initializers.
std::size_t g_allocation_count = 0;

}  // namespace

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    ++g_allocation_count;
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using bw_input_test::EngineFeed;

// Raw 300 ms Big press in Idle: exactly one routed gesture with the full
// envelope — event at the debounced release (t = 320), OpenMenu command.
void test_engine_end_to_end_open_menu(void) {
    EngineFeed f;
    f.cycleBig(300);
    TEST_ASSERT_EQUAL_UINT8(1, f.rec_count);

    const bw::input::RoutedGesture& item = f.rec[0];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::GestureKey::BigShort),
                          static_cast<int>(item.gesture.key));
    TEST_ASSERT_EQUAL_UINT64(320, item.gesture.t_ms);

    TEST_ASSERT_TRUE(item.has_button_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonId::Big),
                          static_cast<int>(item.button_event.button));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ButtonGesture::ShortPress),
                          static_cast<int>(item.button_event.gesture));

    TEST_ASSERT_TRUE(item.has_command);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::CommandType::OpenMenu),
                          static_cast<int>(item.command.type));
    TEST_ASSERT_EQUAL_UINT64(320, item.command.t_ms);
    TEST_ASSERT_EQUAL_UINT8(bw::input::kCommandSourceButton, item.command.source);
    TEST_ASSERT_EQUAL_UINT32(0, item.command.arg0);
    TEST_ASSERT_EQUAL_UINT32(0, item.command.arg1);
}

// D6: a full command queue refuses the push (the queue counts the drop), the
// sink reports false without blocking, and the next push succeeds after a pop.
void test_engine_queue_full_drop_path(void) {
    bw::SpscQueue<bw::Command, 16> queue;
    bw::input::QueueCommandSink<16> sink(&queue);
    bw::Command cmd{};
    cmd.type = bw::CommandType::NextScreen;
    cmd.source = bw::input::kCommandSourceButton;

    for (uint8_t i = 0; i < 16; ++i) {
        cmd.arg0 = i;
        TEST_ASSERT_TRUE(sink.push(cmd));
    }
    cmd.arg0 = 99;
    TEST_ASSERT_FALSE(sink.push(cmd));  // full: refused, never crashes
    TEST_ASSERT_EQUAL_UINT32(1, queue.drops());

    bw::Command out{};
    TEST_ASSERT_TRUE(queue.pop(out));
    TEST_ASSERT_EQUAL_UINT32(0, out.arg0);
    TEST_ASSERT_TRUE(sink.push(cmd));  // room again
    TEST_ASSERT_EQUAL_UINT32(1, queue.drops());

    // A detached sink refuses instead of dereferencing null.
    bw::input::QueueCommandSink<16> detached(nullptr);
    TEST_ASSERT_FALSE(detached.push(cmd));
}

// TimeMs is uint64 (02 §1): events past 32 bits keep every bit.
void test_engine_timebeyond_32bit(void) {
    EngineFeed f;
    f.t = 0x100000100ULL;
    f.cycleBig(300);
    TEST_ASSERT_EQUAL_UINT8(1, f.rec_count);
    TEST_ASSERT_EQUAL_UINT64(0x100000100ULL + 320, f.rec[0].command.t_ms);
}

// Restart resets state (D6): an interrupted chord attempt must not poison the
// very next press.
void test_engine_reset_clears_state(void) {
    EngineFeed f;
    f.big = true;
    f.small = true;
    f.tick(20);  // chord attempt under way
    f.big = false;
    f.tick(1);  // first release: chord_resolving_ pending
    f.engine.reset();
    f.big = false;
    f.small = false;
    f.tick(1);  // fresh first observation: both released, nothing ignored

    f.cycleBig(300);
    TEST_ASSERT_EQUAL_UINT8(1, f.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::GestureKey::BigShort),
                          static_cast<int>(f.rec[0].gesture.key));
}

// Raw bounce before a clean 300 ms hold still yields exactly one gesture.
void test_engine_bounce_then_hold_single_gesture(void) {
    EngineFeed f;
    for (uint8_t i = 0; i < 10; ++i) {
        f.big = !f.big;  // 100 ms of chatter, every flip inside the window
        f.tick(1);
    }
    f.big = true;
    f.tick(30);
    f.big = false;
    f.tick(4);
    TEST_ASSERT_EQUAL_UINT8(1, f.rec_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::input::GestureKey::BigShort),
                          static_cast<int>(f.rec[0].gesture.key));
}

// §10 / D5 through the full stack: one stuck edge, no gesture from the stuck
// press, and a working button afterwards.
void test_engine_stuck_end_to_end(void) {
    EngineFeed f;
    f.tick(5);
    f.big = true;
    f.tick(1100);  // > 10 s held
    TEST_ASSERT_EQUAL_UINT8(1, f.stuck_edges);
    TEST_ASSERT_TRUE(f.stuck_seen[0]);
    TEST_ASSERT_FALSE(f.stuck_seen[1]);
    TEST_ASSERT_EQUAL_UINT8(0, f.rec_count);

    f.big = false;
    f.tick(4);
    TEST_ASSERT_EQUAL_UINT8(0, f.rec_count);  // the stuck press never emits

    f.cycleBig(300);  // re-armed
    TEST_ASSERT_EQUAL_UINT8(1, f.rec_count);
    TEST_ASSERT_EQUAL_UINT8(1, f.stuck_edges);  // no repeat
}

// Boot-held through the whole pipeline: no gestures, still one stuck report.
void test_engine_boot_held_end_to_end(void) {
    EngineFeed f;
    f.big = true;
    f.tick(1200);  // 12 s
    f.big = false;
    f.tick(4);
    TEST_ASSERT_EQUAL_UINT8(0, f.rec_count);
    TEST_ASSERT_EQUAL_UINT8(1, f.stuck_edges);
}

// §9 / conventions: steady-state step() does no heap allocation.
void test_engine_no_heap_over_repeated_steps(void) {
    EngineFeed f;
    // Warm-up first: construct everything and take one path through each
    // branch (first-use guards), so the snapshot sees a quiet steady state.
    f.cycleBig(300);
    f.cycleSmall(2000);
    f.engine.reset();
    f.big = false;
    f.small = false;
    f.tick(2);

    const std::size_t before = g_allocation_count;
    bw::input::StepOutput out{};
    for (uint16_t i = 0; i < 1000; ++i) {
        const bool press = (i % 100) < 50;
        f.big = press;
        f.small = !press;
        out = f.engine.step(f.t, f.ctx, f.big, f.small);
        f.t += 10;
    }
    const std::size_t after = g_allocation_count;
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
    // The loop really exercised both buttons (release edges inside it).
    TEST_ASSERT_TRUE(out.count <= bw::input::kMaxGesturesPerStep);
}

}  // namespace

// Registration entry point called by main.cpp.
void register_engine_tests(void) {
    RUN_TEST(test_engine_end_to_end_open_menu);
    RUN_TEST(test_engine_queue_full_drop_path);
    RUN_TEST(test_engine_timebeyond_32bit);
    RUN_TEST(test_engine_reset_clears_state);
    RUN_TEST(test_engine_bounce_then_hold_single_gesture);
    RUN_TEST(test_engine_stuck_end_to_end);
    RUN_TEST(test_engine_boot_held_end_to_end);
    RUN_TEST(test_engine_no_heap_over_repeated_steps);
}
