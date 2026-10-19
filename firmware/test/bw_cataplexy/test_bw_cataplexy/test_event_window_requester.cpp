// firmware/test/bw_cataplexy/test_bw_cataplexy/test_event_window_requester.cpp —
// F09 §9: window-capture boundaries (pre/post) and the Research-mode gate (§5).

#include <unity.h>

#include <bw/cataplexy/cataplexy_config.h>
#include <bw/cataplexy/event_window_requester.h>

#include "fixtures.h"

namespace {

using bw::cataplexy::CaptureWindow;
using bw::cataplexy::EventWindowRequester;
using bw::cataplexy::kPostEventCaptureMs;
using bw::cataplexy::kPreEventCaptureMs;
using bw::cataplexy::reasonBit;

constexpr bw::TimeMs kMarkMs = 100000;

}  // namespace

void test_window_idle_until_marked(void) {
    EventWindowRequester requester;
    CaptureWindow window;

    TEST_ASSERT_FALSE(requester.pending());
    TEST_ASSERT_FALSE(
        requester.step(kMarkMs + kPostEventCaptureMs, bw::OperatingMode::Research, window));
}

void test_window_not_due_before_post_window(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));

    TEST_ASSERT_FALSE(
        requester.step(kMarkMs + kPostEventCaptureMs - 1, bw::OperatingMode::Research, window));
    TEST_ASSERT_TRUE(requester.pending());
}

void test_window_due_exactly_at_post_boundary(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));

    TEST_ASSERT_TRUE(
        requester.step(kMarkMs + kPostEventCaptureMs, bw::OperatingMode::Research, window));
    TEST_ASSERT_EQUAL_UINT64(kMarkMs, window.mark_ms);
    TEST_ASSERT_EQUAL_UINT64(kMarkMs - kPreEventCaptureMs, window.t_start_ms);
    TEST_ASSERT_EQUAL_UINT64(kMarkMs + kPostEventCaptureMs, window.t_end_ms);
    TEST_ASSERT_BITS_HIGH(reasonBit(bw::ReasonCode::UserMarked), window.reasons);
    TEST_ASSERT_FALSE(requester.pending());  // the capture was handed out
}

void test_window_clamps_start_at_boot_boundary(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    const bw::TimeMs early_mark = kPreEventCaptureMs / 2;  // inside the pre-window
    TEST_ASSERT_TRUE(requester.onMark(early_mark));

    TEST_ASSERT_TRUE(
        requester.step(early_mark + kPostEventCaptureMs, bw::OperatingMode::Research, window));
    TEST_ASSERT_EQUAL_UINT64(0, window.t_start_ms);  // TimeMs 0 is boot (02 §1)
    TEST_ASSERT_EQUAL_UINT64(early_mark + kPostEventCaptureMs, window.t_end_ms);
}

void test_window_dropped_outside_research_mode(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));

    TEST_ASSERT_FALSE(
        requester.step(kMarkMs + kPostEventCaptureMs, bw::OperatingMode::Day, window));
    TEST_ASSERT_FALSE(requester.pending());  // dropped, never retried later (spec §5)
    TEST_ASSERT_FALSE(
        requester.step(kMarkMs + 2 * kPostEventCaptureMs, bw::OperatingMode::Research, window));
}

void test_window_second_mark_ignored_while_pending(void) {
    EventWindowRequester requester;
    CaptureWindow window;

    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));
    TEST_ASSERT_FALSE(requester.onMark(kMarkMs + 5000));  // one capture in flight
    TEST_ASSERT_EQUAL_UINT64(kMarkMs, requester.markMs());

    TEST_ASSERT_TRUE(
        requester.step(kMarkMs + kPostEventCaptureMs, bw::OperatingMode::Research, window));
    TEST_ASSERT_EQUAL_UINT64(kMarkMs, window.mark_ms);

    TEST_ASSERT_TRUE(requester.onMark(kMarkMs + kPostEventCaptureMs + 1));  // free again
}

void test_window_reset_clears_pending(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));

    requester.reset();

    TEST_ASSERT_FALSE(requester.pending());
    TEST_ASSERT_FALSE(
        requester.step(kMarkMs + kPostEventCaptureMs, bw::OperatingMode::Research, window));
}

void test_window_backwards_clock_is_not_due(void) {
    EventWindowRequester requester;
    CaptureWindow window;
    TEST_ASSERT_TRUE(requester.onMark(kMarkMs));

    // Unsigned subtraction must not read a backwards timestamp as "due".
    TEST_ASSERT_FALSE(requester.step(kMarkMs - 1000, bw::OperatingMode::Research, window));
    TEST_ASSERT_TRUE(requester.pending());
}
