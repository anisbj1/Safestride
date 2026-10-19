// F08 §9 test_events_and_status.cpp — the emitted Event contract (§6 D7
// payload layout, §3 open marker via the sink path) and the live EpisodeStatus
// mailbox guard (§6 D6/CR-007): status_changed only on phase/t_start change.

#include <type_traits>

#include <bw/containers/latest_value.h>
#include <bw/events.h>
#include <bw/sleep/sleep_episode_tracker.h>
#include <bw/state.h>
#include <bw/testkit/fake_clock.h>
#include <unity.h>

#include "sleep_test_support.h"

using bw::EpisodeClass;
using bw::EpisodePhase;
using bw::TimeMs;
using bw::sleep::SleepEpisodeConfig;
using bw::sleep::SleepEpisodeTracker;
using bw::sleep::StepOutput;
using bw::testkit::FakeClock;
using f08_test::awake;
using f08_test::makeSnapshot;
using f08_test::RecordingEventSink;

namespace {

constexpr SleepEpisodeConfig kCfg{};
constexpr TimeMs kT0 = 1000;
constexpr TimeMs kPromoteT = kT0 + kCfg.kMinEpisodeMs;  // 5 min span ⇒ Active
constexpr uint8_t kFlags = bw::sleep::SleepEpisodeConfig::kPayloadFlagsIndex;
constexpr uint8_t kClass = bw::sleep::SleepEpisodeConfig::kPayloadClassIndex;
constexpr uint8_t kShare = bw::sleep::SleepEpisodeConfig::kPayloadShareIndex;

StepOutput sleepStep(SleepEpisodeTracker& t, TimeMs t_ms) {
    return t.step(makeSnapshot(t_ms));
}

}  // namespace

// ---- open-marker event (§3, §6 D1 row 3, §6 D7) ---------------------------------

void test_events_open_marker_fields(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput promoted = sleepStep(t, kPromoteT);
    TEST_ASSERT_TRUE(promoted.has_event);
    const bw::Event& e = promoted.event;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::SleepEpisode), static_cast<int>(e.type));
    TEST_ASSERT_EQUAL_UINT8(kCfg.kSleepEpisodeDetectorId, e.detector_id);
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, e.schema_version);
    TEST_ASSERT_EQUAL_UINT8(kCfg.kFlagOpenMarker, e.payload[kFlags]);
    // Wall time unknown ⇒ the provisional class follows row 4 honestly.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Uncertain), e.payload[kClass]);
    TEST_ASSERT_EQUAL_UINT8(0, e.payload[kShare]);  // all observed time was sleep-like
    // Reserved payload bytes stay 0 (§6 D7) and F17-owned fields are untouched.
    for (uint8_t i = 3; i < bw::Event::kMaxPayloadBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, e.payload[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, e.seq);
    TEST_ASSERT_EQUAL_UINT32(0, e.boot_id);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Unlabeled), static_cast<int>(e.label));
    // The episode's start context (§6 D1 row 2).
    TEST_ASSERT_EQUAL_UINT64(kT0, e.t_start_ms);
    TEST_ASSERT_EQUAL_UINT64(kCfg.kMinEpisodeMs, e.duration_ms);  // span-so-far
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::AlertnessState::PossibleSleep),
                          static_cast<int>(e.alertness_at_start));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ActivityContext::LyingResting),
                          static_cast<int>(e.context));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good), static_cast<int>(e.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good), static_cast<int>(e.quality.ppg));
}

void test_events_close_cleared_flags_and_final_class(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    sleepStep(t, kPromoteT);
    StepOutput closed = t.step(awake(kPromoteT + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    const bw::Event& e = closed.event;
    TEST_ASSERT_EQUAL_UINT8(0, e.payload[kFlags]);  // closed: neither open nor recovered
    // 420 s Day span, no clock ⇒ Uncertain (row 4): the honest close class.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Uncertain), e.payload[kClass]);
    TEST_ASSERT_EQUAL_UINT32(0, e.t_start_unix);  // never synced
    TEST_ASSERT_EQUAL_UINT64(kCfg.kMinEpisodeMs + kCfg.kMergeGapMs + 1, e.duration_ms);
    TEST_ASSERT_EQUAL_UINT32(0, e.reasons);  // ordinary close: no extra reason
}

// ---- §9: provisional vs final class bytes -----------------------------------------

void test_events_provisional_vs_final_class_bytes(void) {
    // Synced clock, Day mode: the 5 min open marker is provisionally Rest, the
    // 4 h 10 min close is finally MainSleep (§6 D7: open carries the
    // provisional class, close the final one).
    FakeClock clock;
    clock.setUnixSeconds(100000);
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    sleepStep(t, kT0);
    for (TimeMs t_ms = kT0 + 60000; t_ms < kT0 + 250u * 60u * 1000u; t_ms += 60000) {
        sleepStep(t, t_ms);
    }
    StepOutput closed = t.step(awake(kT0 + 250u * 60u * 1000u + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    // The open marker was captured during the scripted loop; re-derive its
    // class here: 5 min span, share 0, Day, synced ⇒ Rest.
    SleepEpisodeTracker probe{kCfg, nullptr, &clock};
    sleepStep(probe, kT0);
    StepOutput promoted = sleepStep(probe, kPromoteT);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Rest),
                            promoted.event.payload[kClass]);
    // Final: duration ≥ kNapMaxMs ⇒ MainSleep (row 2 beats mode/time).
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::MainSleep),
                            closed.event.payload[kClass]);
    TEST_ASSERT_EQUAL_UINT32(100000, closed.event.t_start_unix);
}

// ---- t_start_unix sampled once, at candidate start --------------------------------

void test_events_t_start_unix_captured_at_candidate_start(void) {
    FakeClock clock;  // starts unknown (unixSeconds() == 0, DEC-009)
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    sleepStep(t, kT0);          // candidate starts with the clock UNSYNCED
    clock.setUnixSeconds(555);  // phone syncs mid-candidate — must not leak back
    StepOutput promoted = sleepStep(t, kPromoteT);
    TEST_ASSERT_EQUAL_UINT32(0, promoted.event.t_start_unix);
    StepOutput closed = t.step(awake(kPromoteT + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_EQUAL_UINT32(0, closed.event.t_start_unix);
}

void test_events_t_start_unix_zero_when_never_synced(void) {
    FakeClock clock;  // present but never synced
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    sleepStep(t, kT0);
    StepOutput promoted = sleepStep(t, kPromoteT);
    TEST_ASSERT_EQUAL_UINT32(0, promoted.event.t_start_unix);
    // …and the class follows the unknown-clock row.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Uncertain),
                            promoted.event.payload[kClass]);
}

// ---- degraded share / confidence (§6 D5) -------------------------------------------

void test_events_share_exactly_max_is_not_uncertain(void) {
    // §8/§9 boundary: 300 s sleep-like out of 600 s observed = share exactly
    // kMaxDegradedShare → row 1 does NOT fire (`>`); Day + synced ⇒ Rest.
    FakeClock clock;
    clock.setUnixSeconds(100000);
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    sleepStep(t, kT0);
    sleepStep(t, kPromoteT);                          // total 300 s, all sleep-like
    t.step(awake(kT0 + 420000));                      // gap == kMergeGapMs: merged
    StepOutput closed = t.step(awake(kT0 + 600000));  // total 600 s ⇒ share 0.5
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Rest), closed.event.payload[kClass]);
    TEST_ASSERT_EQUAL_UINT8(50, closed.event.payload[kShare]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, closed.event.confidence);  // 0.8 × (1 − 0.5)
}

void test_events_share_epsilon_over_max_is_uncertain(void) {
    FakeClock clock;
    clock.setUnixSeconds(100000);
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    sleepStep(t, kT0);
    sleepStep(t, kPromoteT);
    t.step(awake(kT0 + 420000));
    StepOutput closed = t.step(awake(kT0 + 600002));  // total 600 002 ⇒ share > 0.5
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Uncertain),
                            closed.event.payload[kClass]);
}

void test_events_confidence_is_mean_times_one_minus_share(void) {
    // All per-snapshot confidences are 0.8 (mean 0.8); observed 540 001 ms of
    // which 300 000 ms sleep-like ⇒ share ≈ 0.4444, confidence ≈ 0.4444.
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    sleepStep(t, kPromoteT);
    t.step(awake(kT0 + 420000));                      // merged (gap == kMergeGapMs)
    StepOutput closed = t.step(awake(kT0 + 540001));  // gap 240001 ⇒ close
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4444f, closed.event.confidence);
    TEST_ASSERT_EQUAL_UINT8(44, closed.event.payload[kShare]);
}

// ---- status_changed guard (§6 D6, protects LatestValue::overwrites()) -------------

void test_status_changed_only_on_phase_or_start_change(void) {
    SleepEpisodeTracker t{kCfg};
    TEST_ASSERT_TRUE(sleepStep(t, kT0).status_changed);                  // Idle → Candidate
    TEST_ASSERT_FALSE(sleepStep(t, kT0 + 60000).status_changed);         // continues
    TEST_ASSERT_FALSE(sleepStep(t, kT0 + 60000).status_changed);         // row-1 no-op
    TEST_ASSERT_TRUE(sleepStep(t, kPromoteT).status_changed);            // → Active
    TEST_ASSERT_FALSE(t.step(awake(kPromoteT + 60000)).status_changed);  // merged gap
    TEST_ASSERT_TRUE(
        t.step(awake(kPromoteT + 60000 + kCfg.kMergeGapMs + 1)).status_changed);  // → Idle
}

void test_status_values_across_phases(void) {
    FakeClock clock;
    clock.setUnixSeconds(100000);
    SleepEpisodeTracker t{kCfg, nullptr, &clock};
    StepOutput candidate = sleepStep(t, kT0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(candidate.status.phase));
    TEST_ASSERT_EQUAL_UINT64(kT0, candidate.status.t_start_ms);
    // Provisional while open (02 §4.5): 0 s span, Day, synced ⇒ Rest.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Rest),
                          static_cast<int>(candidate.status.episode_class));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.8f, candidate.status.confidence);

    StepOutput active = sleepStep(t, kPromoteT);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Active),
                          static_cast<int>(active.status.phase));
    TEST_ASSERT_EQUAL_UINT64(kT0, active.status.t_start_ms);

    StepOutput closed = t.step(awake(kPromoteT + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(closed.status.phase));
    TEST_ASSERT_EQUAL_UINT64(0, closed.status.t_start_ms);  // 0 when Idle (02 §4.5)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodeClass::Uncertain),
                          static_cast<int>(closed.status.episode_class));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, closed.status.confidence);
    TEST_ASSERT_EQUAL_UINT64(kPromoteT + kCfg.kMergeGapMs + 1, closed.status.t_ms);
}

// ---- §9: the suite's sink drives the §7 adapter contract ----------------------------

void test_events_delivered_through_sink_pair_open_and_close(void) {
    // Mirrors wire_sleep's delivery: step → if has_event → publish (result
    // ignored, §10: never retried). A rejecting sink must not disturb the
    // machine's bookkeeping.
    RecordingEventSink sink;
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput promoted = sleepStep(t, kPromoteT);
    (void)sink.publish(promoted.event);
    StepOutput closed = t.step(awake(kPromoteT + kCfg.kMergeGapMs + 1));
    (void)sink.publish(closed.event);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(sink.count));
    TEST_ASSERT_EQUAL_UINT8(kCfg.kFlagOpenMarker, sink.at(0).payload[kFlags]);
    TEST_ASSERT_EQUAL_UINT8(0, sink.at(1).payload[kFlags]);

    // §10: with the sink refusing everything, the episode still completes and
    // the marker-side bookkeeping (close, status Idle) is unchanged.
    sink.reject = true;
    SleepEpisodeTracker t2{kCfg};
    sleepStep(t2, kT0);
    StepOutput promoted2 = sleepStep(t2, kPromoteT);
    (void)sink.publish(promoted2.event);  // rejected: Full
    StepOutput closed2 = t2.step(awake(kPromoteT + kCfg.kMergeGapMs + 1));
    (void)sink.publish(closed2.event);                               // rejected: Full
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(sink.count));  // attempts counted
    TEST_ASSERT_TRUE(closed2.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(closed2.status.phase));
}

// ---- uint32 saturation (audit L-2; 02 §4.5: Event::duration_ms is uint32) -------

void test_events_duration_saturates_at_uint32_max(void) {
    // Internal TimeMs is uint64 (01 §4: never truncate); the Event contract's
    // duration_ms is uint32. A span beyond 2^32 ms (~49.7 days) must SATURATE
    // to 0xFFFF'FFFF, never wrap. The span's low 32 bits are 1100000 ≠
    // 0xFFFF'FFFF, so a truncating regression fails this assertion (H10:
    // verified — naive cast yields 1100000, this test fails).
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    (void)sleepStep(t, kPromoteT);                   // Active
    const TimeMs span = (TimeMs{1} << 32) + 800000;  // > uint32 max, wraps if truncated
    StepOutput closed = t.step(awake(kPromoteT + span));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, closed.event.duration_ms);
}

// ---- registry/schema pins (audit L-3; §6 D8, §8) --------------------------------

void test_config_registry_literals_pinned(void) {
    // Pin the literals THEMSELVES (not just their use): detector id 8 is
    // pre-allocated to F08 by the integrator's registry, and marker schema 1
    // is the first on-disk marker version. Tests elsewhere compare against
    // cfg.kSleepEpisodeDetectorId, so a silent default change would stay
    // self-consistent — only a hard-coded pin catches it (compile-time twin:
    // static_assert in bw/sleep/sleep_config.h).
    const SleepEpisodeConfig cfg{};
    TEST_ASSERT_EQUAL_UINT8(8, cfg.kSleepEpisodeDetectorId);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.kMarkerSchemaVersion);
}

// ---- §9 budget: contract structs and tracker size ------------------------------------

void test_budget_contracts_and_size(void) {
    // 02 §1: contract structs must stay trivially copyable (queues/mailboxes).
    static_assert(std::is_trivially_copyable_v<bw::Event>, "Event must stay trivially copyable");
    static_assert(std::is_trivially_copyable_v<bw::StateSnapshot>,
                  "StateSnapshot must stay trivially copyable");
    static_assert(std::is_trivially_copyable_v<bw::EpisodeStatus>,
                  "EpisodeStatus must stay trivially copyable");
    static_assert(std::is_trivially_copyable_v<bw::sleep::OpenMarkerRecord>,
                  "marker record travels through IKeyValueStore bytes");
    static_assert(sizeof(bw::Event) == bw::kEventBytes, "Event layout is pinned (CR-002)");
    // §11: RAM < 1 KB — reported in the status log at Implement.
    TEST_ASSERT_TRUE(sizeof(SleepEpisodeTracker) < 1024);
    TEST_ASSERT_TRUE(sizeof(bw::sleep::OpenMarkerRecord) < 16);
}
