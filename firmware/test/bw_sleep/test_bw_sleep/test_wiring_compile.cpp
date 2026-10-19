// F08 wiring sanity check (additive to §9's five files): compiles
// wire_sleep.cpp into the host suite (README §3 requires the wiring pair to
// build for native too, so P4 finds no breakage) and pins the §8 pump
// constants.

#include "../../../src/app/wiring/wire_sleep.cpp"
#include <unity.h>

void test_wiring_constants_match_spec(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, f08::kSleepPollPeriodMs);    // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(2, f08::kSleepPumpPriority);       // §8 [DERIVED]
    TEST_ASSERT_EQUAL_UINT32(2048, f08::kSleepPumpStackBytes);  // §8 [EXPERIMENTAL]
    TEST_ASSERT_EQUAL_UINT32(1, f08::kSleepPumpCore);           // runtime.md §3.2
}

void test_wiring_wire_is_safe_with_empty_context(void) {
    // Composition-root smoke test: wire() constructs the tracker, binds the
    // (empty) snapshot mailbox and runs recover() once — with no clock, no
    // sink and no status mailbox attached it must do all of that silently
    // (§10: missing dependencies degrade, never crash).
    app::AppContext ctx{};
    ctx.clock = nullptr;
    f08::wire(ctx);
    f08::attachEventSink(nullptr);
    f08::attachEpisodeStatus(nullptr);
    // The tracker exists and starts Idle (no snapshot was ever published).
    bw::StateSnapshot snapshot{};
    TEST_ASSERT_FALSE(f08::tracker().step(snapshot).has_event);
}
