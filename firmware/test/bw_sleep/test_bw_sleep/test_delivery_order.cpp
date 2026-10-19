// F08 audit M-1 — order-independent event delivery (§7 wiring contract).
// wire() runs recover() and starts the pump possibly BEFORE attachEventSink
// runs (the P4 hook is called at the composition root's discretion), so an
// event produced while detached must survive until attach instead of being
// dropped into a null sink. These tests drive the REAL wiring path —
// AppContext mailbox → f08::pumpOnce() → shared tracker → deliver/flush —
// so the M-1 test FAILS if the pending-event buffer is reverted (the event
// would be delivered into a null sink: sink.count stays 0).
//
// Shared-state note: f08::tracker() is the one wiring instance per test
// binary (built in the test_wiring_compile.cpp TU). Each test below first
// settles the machine to Idle with a THROWAWAY sink absorbing any carry-over
// close event, so the assertions do not depend on which test ran before.

#include "../../../src/app/app.h"
#include "../../../src/app/wiring/wire_sleep.h"

#include <bw/sleep/sleep_config.h>
#include <unity.h>

#include "sleep_test_support.h"

namespace {

constexpr bw::sleep::SleepEpisodeConfig kCfg{};
constexpr uint8_t kFlags = bw::sleep::SleepEpisodeConfig::kPayloadFlagsIndex;
constexpr bw::TimeMs kMin = kCfg.kMinEpisodeMs;  // 5 min (§8)
constexpr bw::TimeMs kGap = kCfg.kMergeGapMs;    // 2 min (§8)

// Settle the shared wiring tracker to Idle: with a sink attached, any
// carry-over Active episode is CLOSED into `absorb` (never into the sink the
// test asserts on, and never into the pending buffer). Then detach again —
// that reopens the M-1 window (events produced while detached).
void settleIdleDetached(app::AppContext& ctx, bw::TimeMs settle_t,
                        f08_test::RecordingEventSink& absorb) {
    f08::attachEventSink(&absorb);
    ctx.state_snapshot.set(f08_test::awake(settle_t));
    f08::pumpOnce();  // Active → close into absorb; Idle → no-op
    f08::attachEventSink(nullptr);
}

}  // namespace

// M-1 core: the event is produced while NO sink is attached and must be
// delivered when the sink attaches afterwards (audit: the boot recover()
// event was silently dropped once F18 lands a KV store).
void test_wire_event_buffered_until_sink_attaches(void) {
    app::AppContext ctx{};
    ctx.clock = nullptr;

    // The audit's exact scenario: wire() (recover() + pump start) with the
    // P4 attach hook NOT yet run. recover() yields nothing here (no KV store
    // in the W2 build, §6 D4), but it shares the same deliver() path the
    // pumped event below exercises.
    f08::attachEventSink(nullptr);
    f08::wire(ctx);

    f08_test::RecordingEventSink absorb;
    settleIdleDetached(ctx, 1000, absorb);

    // Produce exactly one event while detached: Candidate → Active promotion
    // (open marker, §6 D1 row 3).
    ctx.state_snapshot.set(f08_test::makeSnapshot(2000));
    f08::pumpOnce();  // Candidate
    ctx.state_snapshot.set(f08_test::makeSnapshot(2000 + kMin));
    f08::pumpOnce();  // Active → open-marker event → buffered, NOT dropped

    f08_test::RecordingEventSink sink;
    f08::attachEventSink(&sink);  // late attach flushes the buffer in order

    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(sink.count));
    TEST_ASSERT_EQUAL_UINT8(kCfg.kFlagOpenMarker, sink.at(0).payload[kFlags]);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMin),
                             sink.at(0).duration_ms);    // span-so-far, intact
    TEST_ASSERT_EQUAL_UINT8(8, sink.at(0).detector_id);  // registry pin (§6 D8)

    // Cleanup: leave the pending buffer empty and the tracker detached.
    f08::attachEventSink(nullptr);
}

// The other attach order (regression guard): sink attached FIRST, both the
// promote and the close events publish directly — no buffering involved.
void test_wire_event_published_directly_when_sink_attaches_first(void) {
    app::AppContext ctx{};
    ctx.clock = nullptr;
    f08::wire(ctx);  // idempotent; recover() is once-per-boot (already spent)

    f08_test::RecordingEventSink absorb;
    settleIdleDetached(ctx, 500000, absorb);

    f08_test::RecordingEventSink sink;
    f08::attachEventSink(&sink);  // standard P4 order: attach BEFORE events

    // One full episode with the sink already attached (§9 happy path).
    ctx.state_snapshot.set(f08_test::makeSnapshot(501000));
    f08::pumpOnce();  // Candidate
    ctx.state_snapshot.set(f08_test::makeSnapshot(501000 + kMin));
    f08::pumpOnce();  // Active → open marker, published directly
    ctx.state_snapshot.set(f08_test::awake(501000 + kMin + kGap + 1));
    f08::pumpOnce();  // rows 6: gap > kMergeGapMs → close, published directly

    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(sink.count));
    TEST_ASSERT_EQUAL_UINT8(kCfg.kFlagOpenMarker, sink.at(0).payload[kFlags]);
    TEST_ASSERT_EQUAL_UINT8(0, sink.at(1).payload[kFlags]);  // close: flags cleared

    // The attach-first order buffered nothing: re-attaching flushes zero.
    f08::attachEventSink(nullptr);
    f08_test::RecordingEventSink later;
    f08::attachEventSink(&later);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(later.count));
    f08::attachEventSink(nullptr);
}
