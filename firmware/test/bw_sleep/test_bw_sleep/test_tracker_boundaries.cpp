// F08 §9 test_tracker_boundaries.cpp — every row of the §6 D1 transition
// table in order, the §8 inclusive boundaries (kMinEpisodeMs −1/=/+1;
// kMergeGapMs exact/+1), NotWorn interruptions, de-duplication and the
// backwards-t_ms discontinuity. Scripted snapshots with explicit t_ms.

#include <bw/sleep/sleep_episode_tracker.h>
#include <bw/state.h>
#include <unity.h>

#include "sleep_test_support.h"

using bw::EpisodePhase;
using bw::TimeMs;
using bw::sleep::SleepEpisodeConfig;
using bw::sleep::SleepEpisodeTracker;
using bw::sleep::StepOutput;
using f08_test::awake;
using f08_test::makeSnapshot;
using f08_test::removed;

namespace {

constexpr SleepEpisodeConfig kCfg{};
constexpr TimeMs kT0 = 1000;

// Drives a sleep-like snapshot at `t` (F07 PossibleSleep, worn, healthy data).
StepOutput sleepStep(SleepEpisodeTracker& t, TimeMs t_ms,
                     bw::OperatingMode mode = bw::OperatingMode::Day) {
    return t.step(makeSnapshot(bw::TimeMs{t_ms}, bw::AlertnessState::PossibleSleep,
                               bw::WearState::Worn, mode));
}

}  // namespace

// ---- row 1: duplicate t_ms is a no-op ----------------------------------------

void test_tracker_row1_identical_t_ms_is_noop(void) {
    SleepEpisodeTracker t{kCfg};
    StepOutput first = sleepStep(t, kT0);
    TEST_ASSERT_TRUE(first.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(first.status.phase));
    // The wiring polls IStateProvider::latest() at 1 Hz while snapshots may
    // not advance: the repeat adds nothing — no event, no status write
    // (LatestValue::overwrites() must stay 0).
    StepOutput dup = sleepStep(t, kT0);
    TEST_ASSERT_FALSE(dup.has_event);
    TEST_ASSERT_FALSE(dup.status_changed);
}

// ---- row 2: Idle + PossibleSleep → Candidate ---------------------------------

void test_tracker_row2_idle_to_candidate_records_start(void) {
    SleepEpisodeTracker t{kCfg};
    // Awake first: nothing starts, nothing is published.
    StepOutput idle = t.step(awake(kT0));
    TEST_ASSERT_FALSE(idle.has_event);
    TEST_ASSERT_FALSE(idle.status_changed);
    StepOutput start = sleepStep(t, kT0 + 5000);
    TEST_ASSERT_TRUE(start.status_changed);
    TEST_ASSERT_FALSE(start.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(start.status.phase));
    TEST_ASSERT_EQUAL_UINT64(kT0 + 5000, start.status.t_start_ms);
}

// ---- row 3: span ≥ kMinEpisodeMs → Active (inclusive boundary) ----------------

void test_tracker_row3_promotes_at_exact_min(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput promoted = sleepStep(t, kT0 + kCfg.kMinEpisodeMs);  // span == kMinEpisodeMs
    TEST_ASSERT_TRUE(promoted.has_event);
    TEST_ASSERT_TRUE(promoted.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Active),
                          static_cast<int>(promoted.status.phase));
    TEST_ASSERT_EQUAL_UINT8(
        kCfg.kFlagOpenMarker,
        promoted.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadFlagsIndex]);
}

void test_tracker_row3_min_minus_one_stays_candidate(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput below = sleepStep(t, kT0 + kCfg.kMinEpisodeMs - 1);
    TEST_ASSERT_FALSE(below.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(below.status.phase));
    // …and one millisecond later it promotes: `≥` is inclusive.
    StepOutput at = sleepStep(t, kT0 + kCfg.kMinEpisodeMs);
    TEST_ASSERT_TRUE(at.has_event);
}

// ---- row 4: Candidate + gap > kMergeGapMs → Idle, silently --------------------

void test_tracker_row4_candidate_gap_plus_one_aborts_silently(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    // gap == kMergeGapMs + 1 ms, for the ordinary reason (an awakening).
    StepOutput aborted = t.step(awake(kT0 + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_FALSE(aborted.has_event);  // drowsiness/inactivity, not an episode
    TEST_ASSERT_TRUE(aborted.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(aborted.status.phase));
    // The machine restarts cleanly from the next sleep-like snapshot.
    StepOutput restart = sleepStep(t, kT0 + kCfg.kMergeGapMs + 2000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(restart.status.phase));
}

void test_tracker_row4_candidate_gap_exact_merges(void) {
    // §8 boundary: a gap of exactly kMergeGapMs merges (`≤` merges). The gap
    // is time since the LAST SLEEP-LIKE snapshot (§6 D1), so a returning
    // snapshot must land within kMergeGapMs of it.
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput blip = t.step(awake(kT0 + 60000));
    TEST_ASSERT_FALSE(blip.has_event);
    StepOutput back = sleepStep(t, kT0 + kCfg.kMergeGapMs);  // gap == kMergeGapMs
    TEST_ASSERT_FALSE(back.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(back.status.phase));
    // Second blip, again exactly at the boundary.
    t.step(awake(kT0 + kCfg.kMergeGapMs + 60000));
    StepOutput back2 = sleepStep(t, kT0 + 2 * kCfg.kMergeGapMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(back2.status.phase));
    // Promotion measures the span from the ORIGINAL candidate start, merged
    // gaps included (§6 D1: duration = span).
    StepOutput promoted = sleepStep(t, kT0 + kCfg.kMinEpisodeMs);
    TEST_ASSERT_TRUE(promoted.has_event);
    TEST_ASSERT_EQUAL_UINT64(kT0, promoted.event.t_start_ms);
}

void test_tracker_row4_observed_wakefulness_is_the_gap(void) {
    // §6 D1's gap is time between snapshots whose alertness is not
    // PossibleSleep: OBSERVED wakefulness. A stale (silent) stretch produces
    // no snapshots — it is degraded time (§6 D5), never a gap. So a 60 s
    // observed blip merges, a 3 min unobserved silence does not split, and
    // the promotion qualifier "every gap ≤ kMergeGapMs" is about observed
    // gaps, which rows 4/6 already bound.
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput blip = t.step(awake(kT0 + 60000));  // observed 60 s gap: merges
    TEST_ASSERT_FALSE(blip.has_event);
    StepOutput back = sleepStep(t, kT0 + 240000);  // gap ends; 180 s were unobserved
    TEST_ASSERT_FALSE(back.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(back.status.phase));
    StepOutput promoted = sleepStep(t, kT0 + kCfg.kMinEpisodeMs);  // span == kMinEpisodeMs
    TEST_ASSERT_TRUE(promoted.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Active),
                          static_cast<int>(promoted.status.phase));
    // The unobserved stretch is honestly degraded, not sleep: the open
    // marker's share byte is 60 000 / 300 000 = 20 %.
    TEST_ASSERT_EQUAL_UINT8(
        20, promoted.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadShareIndex]);
}

// ---- row 5: Active + return within the gap → same episode ---------------------

void test_tracker_row5_active_return_continues_same_episode(void) {
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    sleepStep(t, kT0 + kCfg.kMinEpisodeMs);  // Active, open marker
    StepOutput during = t.step(awake(kT0 + kCfg.kMinEpisodeMs + 60000));
    TEST_ASSERT_FALSE(during.has_event);  // no second marker, no close
    StepOutput back = sleepStep(t, kT0 + kCfg.kMinEpisodeMs + 100000);
    TEST_ASSERT_FALSE(back.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Active),
                          static_cast<int>(back.status.phase));
    // Exactly one close at the end.
    StepOutput closed = t.step(awake(kT0 + kCfg.kMinEpisodeMs + 100000 + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_EQUAL_UINT8(
        0, closed.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadFlagsIndex]);
}

// ---- row 6: Active + gap > kMergeGapMs → close --------------------------------

void test_tracker_row6_gap_plus_one_closes(void) {
    SleepEpisodeTracker t{kCfg};
    const TimeMs promote_t = kT0 + kCfg.kMinEpisodeMs;
    sleepStep(t, kT0);
    sleepStep(t, promote_t);
    // Gap from the last sleep-like snapshot is kMergeGapMs + 1.
    StepOutput closed = t.step(awake(promote_t + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_TRUE(closed.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(closed.status.phase));
    // Duration is the span from candidate start (merged gaps included).
    TEST_ASSERT_EQUAL_UINT64(promote_t + kCfg.kMergeGapMs + 1 - kT0, closed.event.duration_ms);
    TEST_ASSERT_EQUAL_UINT64(kT0, closed.event.t_start_ms);
}

// ---- row 7: NotWorn when the gap closes → DeviceNotWorn -----------------------

void test_tracker_row7_notworn_close_carries_reason(void) {
    SleepEpisodeTracker t{kCfg};
    const TimeMs promote_t = kT0 + kCfg.kMinEpisodeMs;
    sleepStep(t, kT0);
    sleepStep(t, promote_t);
    StepOutput closed = t.step(removed(promote_t + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_BITS(f08_test::reasonBit(bw::ReasonCode::DeviceNotWorn),
                     f08_test::reasonBit(bw::ReasonCode::DeviceNotWorn), closed.event.reasons);
}

void test_tracker_notworn_interruption_shorter_than_gap_is_kept(void) {
    // §13 (closed): a removal longer than kMergeGapMs closes and keeps the
    // observed part; a SHORT one is ordinary gap time — the episode survives
    // and the observed span is kept.
    SleepEpisodeTracker t{kCfg};
    const TimeMs promote_t = kT0 + kCfg.kMinEpisodeMs;
    sleepStep(t, kT0);
    sleepStep(t, promote_t);
    StepOutput during = t.step(removed(promote_t + 60000));
    TEST_ASSERT_FALSE(during.has_event);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Active),
                          static_cast<int>(during.status.phase));
    StepOutput back = sleepStep(t, promote_t + 100000);
    TEST_ASSERT_FALSE(back.has_event);
    const TimeMs last = promote_t + 100000;
    StepOutput closed = t.step(awake(last + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    // Closed by an ordinary awakening while worn: no DeviceNotWorn bit.
    TEST_ASSERT_BITS(f08_test::reasonBit(bw::ReasonCode::DeviceNotWorn), 0, closed.event.reasons);
    TEST_ASSERT_EQUAL_UINT64(last + kCfg.kMergeGapMs + 1 - kT0, closed.event.duration_ms);
}

// ---- gap merging: several blips stay one episode -------------------------------

void test_tracker_two_merges_stay_one_episode(void) {
    SleepEpisodeTracker t{kCfg};
    const TimeMs promote_t = kT0 + kCfg.kMinEpisodeMs;
    sleepStep(t, kT0);
    sleepStep(t, promote_t);            // Active + open marker
    t.step(awake(promote_t + 60000));   // blip 1 (60 s < 2 min)
    sleepStep(t, promote_t + 100000);   // back within the gap
    t.step(awake(promote_t + 160000));  // blip 2
    sleepStep(t, promote_t + 200000);   // back again
    StepOutput closed = t.step(awake(promote_t + 200000 + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    // One close only: neither blip emitted anything, no second marker exists.
    TEST_ASSERT_EQUAL_UINT64(kT0, closed.event.t_start_ms);
    TEST_ASSERT_EQUAL_UINT64(promote_t + 200000 + kCfg.kMergeGapMs + 1 - kT0,
                             closed.event.duration_ms);
}

// ---- §9: short sleep-like spans never become events ----------------------------

void test_tracker_short_span_emits_nothing(void) {
    // §6 D1: spans shorter than kMinEpisodeMs are recorded as drowsiness, not
    // sleep — "normal inactivity" is represented by the state stream, never
    // by a SleepEpisode. A 4 min sleep-like span (< kMinEpisodeMs = 5 min)
    // followed by sustained awakening: zero events over the whole script.
    SleepEpisodeTracker t{kCfg};
    TimeMs events = 0;
    for (TimeMs t_ms = kT0; t_ms <= kT0 + 4u * 60u * 1000u; t_ms += 60000) {
        events += sleepStep(t, t_ms).has_event ? 1 : 0;
    }
    const TimeMs last_sleep = kT0 + 4u * 60u * 1000u;
    for (TimeMs t_ms = last_sleep + 60000; t_ms <= last_sleep + 10u * 60u * 1000u; t_ms += 60000) {
        events += t.step(awake(t_ms)).has_event ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT64(0, events);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(t.step(awake(last_sleep + 600000)).status.phase));
}

// ---- §9: Insufficient quality is gap time, not sleep ---------------------------

void test_tracker_insufficient_source_counts_as_gap(void) {
    // §6 D1's gap definition: a PossibleSleep snapshot with an unusable source
    // is NOT sleep-like — it can neither start an episode nor merge a gap.
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    StepOutput degraded = t.step(makeSnapshot(
        kT0 + kCfg.kMinEpisodeMs, bw::AlertnessState::PossibleSleep, bw::WearState::Worn,
        bw::OperatingMode::Day, bw::DataQuality::Good, bw::DataQuality::Insufficient));
    TEST_ASSERT_FALSE(degraded.has_event);  // gap from kT0 is kMinEpisodeMs > kMergeGapMs → abort
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(degraded.status.phase));
}

// ---- row 8: t_ms discontinuity ---------------------------------------------------

void test_tracker_row8_backwards_closes_active_uncertain(void) {
    SleepEpisodeTracker t{kCfg};
    const TimeMs promote_t = kT0 + kCfg.kMinEpisodeMs;
    sleepStep(t, kT0);
    sleepStep(t, promote_t);
    // A reboot (or counter reset) reappears as a smaller t_ms: close on the
    // LAST COHERENT time with the honest flags (§6 D1 row 8, §10).
    StepOutput closed = t.step(makeSnapshot(promote_t - 50000));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_TRUE(closed.status_changed);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(bw::EpisodeClass::Uncertain),
        closed.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadClassIndex]);
    TEST_ASSERT_BITS(f08_test::reasonBit(bw::ReasonCode::SensorUnavailable),
                     f08_test::reasonBit(bw::ReasonCode::SensorUnavailable), closed.event.reasons);
    // Duration spans to the last coherent timestamp — never across the break.
    TEST_ASSERT_EQUAL_UINT64(promote_t - kT0, closed.event.duration_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(closed.status.phase));
}

void test_tracker_row8_backwards_candidate_goes_idle_silently(void) {
    // The table lists row 8 from Active; a Candidate has no marker to clear
    // and row 4's precedent publishes nothing — abort to Idle, no event.
    SleepEpisodeTracker t{kCfg};
    sleepStep(t, kT0);
    sleepStep(t, kT0 + 60000);
    StepOutput broken = sleepStep(t, kT0 + 30000);
    TEST_ASSERT_FALSE(broken.has_event);
    TEST_ASSERT_TRUE(broken.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(broken.status.phase));
    // The new timeline is adopted: an episode can start again immediately.
    StepOutput restart = sleepStep(t, kT0 + 31000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Candidate),
                          static_cast<int>(restart.status.phase));
}

// ---- first call / empty state ----------------------------------------------------

void test_tracker_first_call_with_awake_is_noop(void) {
    SleepEpisodeTracker t{kCfg};
    StepOutput first = t.step(awake(kT0));
    TEST_ASSERT_FALSE(first.has_event);
    TEST_ASSERT_FALSE(first.status_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EpisodePhase::Idle),
                          static_cast<int>(first.status.phase));
}

// ---- §9: the exact wave-2 build — both optional deps nullptr ---------------------

void test_tracker_w2_nullptr_deps_full_episode(void) {
    // kv == nullptr AND wall_clock == nullptr: the exact W2 wiring (§13). The
    // machine behaves identically; recovery and wall time degrade silently.
    SleepEpisodeTracker t{kCfg, nullptr, nullptr};
    TEST_ASSERT_FALSE(t.recover().has_event);  // no store ⇒ no marker ⇒ no event

    // 40 min sleep-like span (promotes at kMinEpisodeMs), then a sustained
    // awakening closes it (DoD §14 timeline).
    TimeMs events = 0;
    bw::Event last_event{};
    for (TimeMs t_ms = kT0; t_ms <= kT0 + 40u * 60u * 1000u; t_ms += 60000) {
        StepOutput out = sleepStep(t, t_ms);
        if (out.has_event) {
            ++events;
            last_event = out.event;
        }
    }
    StepOutput closed = t.step(awake(kT0 + 40u * 60u * 1000u + kCfg.kMergeGapMs + 1));
    if (closed.has_event) {
        ++events;
        last_event = closed.event;
    }
    TEST_ASSERT_EQUAL_UINT64(2, events);  // one open marker + one close
    // Wall time unknown ⇒ the close class follows row 4: Uncertain, honestly.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EpisodeClass::Uncertain),
                            last_event.payload[bw::sleep::SleepEpisodeConfig::kPayloadClassIndex]);
    TEST_ASSERT_EQUAL_UINT32(0, last_event.t_start_unix);
}
