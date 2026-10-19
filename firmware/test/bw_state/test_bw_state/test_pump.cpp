// F07 §9 test_pump.cpp — the 1 Hz pump harness (bw::state::StatePump, spec
// D15), driven by a FakeClock exactly like the platform task reads it (D4):
// the harness sets producer time and the tick reads clock.nowMs(). Covers
// (a) the 1 Hz tick driving the engine from real LatestValue mailboxes,
// (b) the feature-quality staleness aging (auditor A-03): a STOPPED F02
// must age back to Insufficient and gate the engine, and (c) D11 publishing:
// transition records go to the sink, absent/rejected ones are counted
// dropped, never blocking, and (d) the P4b quality provider (spec D16): a
// bound provider substitutes for the derivation, its silence ages through
// kFeatureQualityStaleMs, and a never-received provider stays Insufficient —
// four H10 revert-proof cases; plus the FIX-loop-1 cases: recovery through
// the PROVIDER channel (gate falls on provider Good, full recovery dwell),
// a provider gate mid-rung (evidence reset => full rung dwell re-paid), and
// a `true`-without-write provider (must read Insufficient, never the
// zero-initialized Good/Good): each must FAIL when the provider block in
// StatePump::tick() is disabled or the defensive out-param seed is reverted.

#include <bw/state/state_config.h>
#include <bw/state/state_pump.h>
#include <bw/testkit/fake_clock.h>
#include <unity.h>

#include "state_test_support.h"

using bw::AlertnessState;
using bw::state::StatePump;

namespace f07test {

// One healthy 1 Hz producer set at time t (calm alertness, per freshInputs).
bw::WearEstimate wearAt(bw::TimeMs t) {
    bw::WearEstimate w{};
    w.t_ms = t;
    w.state = bw::WearState::Worn;
    w.confidence = 0.9f;
    return w;
}

bw::ActivityEstimate activityAt(bw::TimeMs t) {
    bw::ActivityEstimate a{};
    a.t_ms = t;
    a.context = bw::ActivityContext::Sitting;
    a.confidence = 0.7f;
    return a;
}

bw::AlertnessProbabilities alertnessAt(bw::TimeMs t) {
    const bw::state::StateInputs calm = f07_test::freshInputs(t);
    return calm.alertness;
}

bw::MotionFeatures motionAt(bw::TimeMs t) {
    bw::MotionFeatures f{};
    f.t_ms = t;
    f.quality = bw::DataQuality::Good;
    return f;
}

bw::PpgFeatures ppgAt(bw::TimeMs t) {
    bw::PpgFeatures f{};
    f.t_ms = t;
    f.quality = bw::DataQuality::Good;
    return f;
}

// ---- FIX-loop-1 helpers (test-only named constants) ------------------------
// Drowsy-band alertness epoch for the pump harness (ladder evidence, D7).
// With the default EMA alpha a CONSTANT input lands on itself after the seed
// (same trick as test_ladder.cpp), so ev_drowsy stays exactly at
// kRungEvidence — no EMA arithmetic in expectations.
constexpr float kSleepShare = 0.1f;       // [EXPERIMENTAL] mirrors test_ladder's atEvidence
constexpr float kRungEvidence = 0.65f;    // [EXPERIMENTAL] inside [kDrowsyEnterProb,
                                          // kHighRiskEnterProb): supports Drowsy, never
                                          // HighDrowsinessRisk
constexpr float kInstantEmaAlpha = 1.0f;  // [EXPERIMENTAL] test-only config injection
                                          // (F04-style, §9): every EMA step lands on its
                                          // input, so dwell expectations need no EMA
                                          // arithmetic — the EMA was already seeded by
                                          // the calm phase otherwise (audit FIX loop 1)

bw::AlertnessProbabilities drowsyAlertnessAt(bw::TimeMs t) {
    bw::AlertnessProbabilities a = f07_test::freshInputs(t).alertness;
    a.drowsy = kRungEvidence - kSleepShare;
    a.sleep = kSleepShare;
    return a;
}

// P4b (spec D16) scripted quality provider. The pump port is a plain
// function pointer (no capture), so the script lives at namespace scope and
// the currently-bound harness publishes itself through it (tests run
// sequentially). `active=false` = provider SILENT: returns false, writes
// nothing — distinct from a provider whose axes are Insufficient.
// `writes=false` models a DEFECTIVE provider that returns true WITHOUT
// writing `out` (audit FIX loop 1): the pump's seeded out-param must make
// that read Insufficient, never the zero-initialized Good/Good.
struct ProviderScript {
    bool active = false;
    bool writes = true;
    bw::DataQuality imu = bw::DataQuality::Good;
    bw::DataQuality ppg = bw::DataQuality::Good;
};

ProviderScript* s_bound_script = nullptr;  // the one live harness (see bindProvider)

bool scriptedProvider(bw::SourceQuality& out) {
    if (s_bound_script == nullptr || !s_bound_script->active) {
        return false;
    }
    if (s_bound_script->writes) {
        out.imu = s_bound_script->imu;
        out.ppg = s_bound_script->ppg;
    }
    return true;
}

// The pump harness: every port bound to a local mailbox, time flowing only
// through the FakeClock (the task's pattern: set producers, then read the
// clock). with_wear/with_features model a producer that stopped publishing.
struct PumpHarness {
    bw::testkit::FakeClock clock;
    StatePump pump;
    bw::LatestValue<bw::WearEstimate> wear;
    bw::LatestValue<bw::ActivityEstimate> activity;
    bw::LatestValue<bw::AlertnessProbabilities> alertness;
    bw::LatestValue<bw::TimeMs> wake_check_passed;
    bw::LatestValue<bw::MotionFeatures> motion;
    bw::LatestValue<bw::PpgFeatures> ppg;
    bw::LatestValue<bw::StateSnapshot> snapshot;
    ProviderScript provider{};  // P4b script (D16), read via s_bound_script

    // cfg is a test-only injection seam (F04-style, §9): the mid-rung provider
    // test needs instant evidence landing; the default config is untouched.
    explicit PumpHarness(const bw::state::StateEngineConfig& cfg = bw::state::StateEngineConfig{})
        : pump(cfg) {
        bw::state::PumpPorts& p = pump.ports();
        p.wear = &wear;
        p.activity = &activity;
        p.alertness = &alertness;
        p.wake_check_passed = &wake_check_passed;
        p.motion = &motion;
        p.ppg = &ppg;
        p.snapshot = &snapshot;
    }

    // Hygiene (audit FIX loop 1): a destroyed harness must not leave the
    // namespace-scope script pointer dangling into its dead member.
    ~PumpHarness() {
        if (s_bound_script == &provider) {
            s_bound_script = nullptr;
        }
    }

    // P4b (D16): OPT IN the scripted quality provider. Tests that do not
    // call this keep the absent-provider path, so the original A-03
    // derivation coverage is untouched (the default is nullptr => derive).
    void bindProvider() {
        provider = ProviderScript{};
        s_bound_script = &provider;
        pump.ports().quality_provider = &scriptedProvider;
    }

    // with_wear=false models the wear producer stopped (the mailbox keeps
    // its last value); a wear override feeds a different wear state; same
    // for with_features (F02 stopped). A drowsy override feeds a non-calm
    // alertness epoch (ladder evidence, D7).
    bw::state::StateOutputs tick(bw::TimeMs t, const bw::WearEstimate* wear_override = nullptr,
                                 bool with_features = true,
                                 const bw::AlertnessProbabilities* alertness_override = nullptr,
                                 bool with_alertness = true) {
        clock.setMs(t);
        wear.set(wear_override != nullptr ? *wear_override : wearAt(t));
        activity.set(activityAt(t));
        if (with_alertness) {
            alertness.set(alertness_override != nullptr ? *alertness_override : alertnessAt(t));
        }
        if (with_features) {
            motion.set(motionAt(t));
            ppg.set(ppgAt(t));
        }
        return pump.tick(clock.nowMs());
    }

    // Precondition: at least one tick published a snapshot.
    bw::StateSnapshot last() {
        bw::StateSnapshot s{};
        (void)snapshot.get(s);
        return s;
    }
};

// Feature-private IEventSink fake (host-testing: shared fakes belong to
// bw_testkit; per-suite helpers stay in the suite).
struct RecordingSink final : bw::IEventSink {
    bw::Event events[4]{};
    size_t count = 0;
    bw::Err forced = bw::Err::Ok;

    bw::Result<void> publish(const bw::Event& e) override {
        if (forced != bw::Err::Ok) {
            return bw::Result<void>{forced};
        }
        if (count < 4) {
            events[count++] = e;
        }
        return bw::Result<void>{bw::Err::Ok};
    }
};

}  // namespace f07test

using f07test::PumpHarness;
using f07test::RecordingSink;

// The response mailbox is sequence-consumed once and only passes its
// timestamp freshness check. Drive the pump to Drowsy before each candidate.
void test_pump_wake_check_sequence_timestamp_and_drowsy_gate(void) {
    bw::state::StateEngineConfig cfg;
    cfg.ema_alpha = f07test::kInstantEmaAlpha;
    PumpHarness h(cfg);
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
        (void)h.tick(t);
    }
    for (bw::TimeMs t = 31000; t <= 31000 + bw::state::kDrowsyMinDurationMs; t += 1000) {
        bw::AlertnessProbabilities drowsy = f07test::drowsyAlertnessAt(t);
        (void)h.tick(t, nullptr, true, &drowsy);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(h.last().alertness));

    const bw::TimeMs pass_time = 31000 + bw::state::kDrowsyMinDurationMs + 1000;
    h.wake_check_passed.set(pass_time);
    bw::AlertnessProbabilities drowsy = f07test::drowsyAlertnessAt(pass_time);
    const bw::state::StateOutputs accepted = h.tick(pass_time, nullptr, true, &drowsy);
    TEST_ASSERT_TRUE(accepted.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(accepted.snapshot.alertness));
    TEST_ASSERT_BITS(f07_test::bitOf(bw::ReasonCode::UserMarked), accepted.snapshot.reasons,
                     f07_test::bitOf(bw::ReasonCode::UserMarked));

    // Same sequence is not replayed. Even with sustained strong evidence,
    // the reset requires a complete new Drowsy dwell before re-entry.
    for (bw::TimeMs elapsed = 1000; elapsed <= bw::state::kDrowsyMinDurationMs; elapsed += 1000) {
        drowsy = f07test::drowsyAlertnessAt(pass_time + elapsed);
        const bw::state::StateOutputs repeated =
            h.tick(pass_time + elapsed, nullptr, true, &drowsy);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                              static_cast<int>(repeated.snapshot.alertness));
    }
    drowsy = f07test::drowsyAlertnessAt(pass_time + bw::state::kDrowsyMinDurationMs + 1000);
    const bw::state::StateOutputs reentered =
        h.tick(pass_time + bw::state::kDrowsyMinDurationMs + 1000, nullptr, true, &drowsy);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(reentered.snapshot.alertness));
}
void test_pump_wake_check_rejects_invalid_timestamps_and_non_drowsy(void) {
    const bw::TimeMs cases[] = {bw::state::kInputStaleMs + 1, 1001};
    for (bw::TimeMs candidate : cases) {
        bw::state::StateEngineConfig cfg;
        cfg.ema_alpha = f07test::kInstantEmaAlpha;
        PumpHarness h(cfg);
        // Unknown (startup) must ignore a fresh response.
        h.wake_check_passed.set(0);
        (void)h.tick(0);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(h.last().alertness));
        // Now reach Drowsy, then submit stale or future timestamp.
        for (bw::TimeMs t = 1000; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
            (void)h.tick(t);
        }
        // A fresh check in Awake is not the F07 Warning/Drowsy condition.
        h.wake_check_passed.set(bw::state::kAwakeRecoveryMs);
        (void)h.tick(bw::state::kAwakeRecoveryMs);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                              static_cast<int>(h.last().alertness));
        for (bw::TimeMs t = 31000; t <= 31000 + bw::state::kDrowsyMinDurationMs; t += 1000) {
            bw::AlertnessProbabilities drowsy = f07test::drowsyAlertnessAt(t);
            (void)h.tick(t, nullptr, true, &drowsy);
        }
        const bw::TimeMs now = 31000 + bw::state::kDrowsyMinDurationMs + 1000;
        h.wake_check_passed.set(candidate == 1001 ? now + 1 : now - candidate);
        bw::AlertnessProbabilities drowsy = f07test::drowsyAlertnessAt(now);
        (void)h.tick(now, nullptr, true, &drowsy);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                              static_cast<int>(h.last().alertness));
    }
}

void test_pump_wake_check_does_not_override_notworn_or_unknown(void) {
    PumpHarness h;
    h.wake_check_passed.set(0);
    bw::WearEstimate off = f07test::wearAt(0);
    off.state = bw::WearState::NotWorn;
    (void)h.tick(0, &off);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::NotWorn),
                          static_cast<int>(h.last().alertness));

    // A fresh new publication while G3a is in Unknown still leaves Unknown.
    h.wake_check_passed.set(1000);
    (void)h.tick(1000, nullptr, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(h.last().alertness));
}

void test_pump_short_motion_marker_is_one_tick_and_starts_dwell_at_onset(void) {
    PumpHarness h;
    (void)h.tick(0);
    const bw::TimeMs onset = 1000;
    bw::AlertnessProbabilities still = f07test::drowsyAlertnessAt(onset);
    still.reasons = f07_test::bitOf(bw::ReasonCode::ShortMotionBurst);
    (void)h.tick(onset, nullptr, true, &still);

    // No further alertness publication: the held last value still carries
    // ShortMotionBurst in its mask, but the pump must present no second pulse.
    for (bw::TimeMs t = onset + 1000; t < onset + bw::state::kDrowsyMinDurationMs; t += 1000) {
        const bw::state::StateOutputs out = h.tick(t, nullptr, true, nullptr, false);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
    }
    const bw::state::StateOutputs out =
        h.tick(onset + bw::state::kDrowsyMinDurationMs, nullptr, true, nullptr, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.snapshot.alertness));
}

// (A-03(1)) The 1 Hz tick drives the engine end-to-end through the real
// mailboxes: boot-Unknown, then Awake exactly kAwakeRecoveryMs of calm after
// the first tick, one transition, lastRunMs tracking the clock.
void test_pump_1hz_drives_engine_from_mailboxes(void) {
    PumpHarness h;
    uint32_t transitions = 0;
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
        const bw::state::StateOutputs out = h.tick(t);
        if (out.transitioned) {
            ++transitions;
        }
    }
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(1, transitions);  // only Unknown -> Awake
    TEST_ASSERT_EQUAL_UINT32(bw::state::kAwakeRecoveryMs, h.pump.lastRunMs());
    TEST_ASSERT_EQUAL_UINT32(0, h.pump.droppedEvents());
}

// (A-03(2)) A STOPPED F02 must not leave its last Good quality latched: no
// new feature window for kFeatureQualityStaleMs ages the channel back to
// Insufficient; with BOTH channels stalled the engine gates to Unknown +
// LowSignalQuality while wear/activity/alertness stay fresh. Exactly at the
// limit is still fresh; one tick more is stale. Fails if the aging fix is
// reverted (the engine would stay Awake on latched Good quality).
void test_pump_stalled_features_age_to_insufficient(void) {
    PumpHarness h;
    // Healthy pipeline to Awake (t <= 30000), running a little longer.
    for (bw::TimeMs t = 0; t <= 35000; t += 1000) {
        (void)h.tick(t);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));

    // F02 stops publishing BOTH feature channels; everything else stays 1 Hz.
    // Boundary: exactly kFeatureQualityStaleMs after the last receipt the
    // quality is still Good and the engine still trusts its state.
    (void)h.tick(35000 + bw::state::kFeatureQualityStaleMs, nullptr, false);
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));

    // One pump tick past the limit: both channels read Insufficient and gate
    // G2 claims Unknown + LowSignalQuality.
    (void)h.tick(35000 + bw::state::kFeatureQualityStaleMs + 1000, nullptr, false);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);
}

// (D11) Transition records are the pump's publication duty: with no sink
// attached a DeviceNotWorn record is COUNTED dropped (never blocking); with
// a sink it is published (type + detector_id intact); a REJECTED publish
// (F17 full) is counted dropped too. No record for Unknown/Awake (D12).
void test_pump_records_published_or_dropped(void) {
    PumpHarness h;
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
        (void)h.tick(t);
    }

    // Take the device off: G1 -> NotWorn escalation record, no sink yet.
    bw::WearEstimate off = f07test::wearAt(31000);
    off.state = bw::WearState::NotWorn;
    (void)h.tick(31000, &off);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::AlertnessState::NotWorn),
                          static_cast<int>(h.last().alertness));
    TEST_ASSERT_EQUAL_UINT32(1, h.pump.droppedEvents());

    // Re-wear + full calm: G3a -> Unknown -> Awake. Both are no-record
    // transitions (D12), so nothing is published or dropped along the way.
    for (bw::TimeMs t = 32000; t <= 62000; t += 1000) {
        (void)h.tick(t);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));
    TEST_ASSERT_EQUAL_UINT32(1, h.pump.droppedEvents());

    // Sink attached: the next escalation is delivered, not dropped.
    RecordingSink sink;
    h.pump.ports().events = &sink;
    off = f07test::wearAt(63000);
    off.state = bw::WearState::NotWorn;
    (void)h.tick(63000, &off);
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::DeviceNotWorn),
                          static_cast<int>(sink.events[0].type));
    TEST_ASSERT_EQUAL_UINT8(bw::state::kStateFeatureId, sink.events[0].detector_id);
    TEST_ASSERT_EQUAL_UINT32(1, h.pump.droppedEvents());

    // Re-wear, then a rejecting sink (F17 Err::Full): the record is counted
    // dropped, the pump never blocks and never retries (D11).
    (void)h.tick(64000);  // wear.Worn again
    sink.forced = bw::Err::Full;
    off = f07test::wearAt(65000);
    off.state = bw::WearState::NotWorn;
    (void)h.tick(65000, &off);  // publish rejected
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);
    TEST_ASSERT_EQUAL_UINT32(2, h.pump.droppedEvents());
}

// ---- P4b quality provider (spec D16, Wave 4; H10 revert-proof) -------------
// Each of these FAILS when the provider block in StatePump::tick() is
// disabled: the provider's value/staleness must reach the quality gate.

// (H10-1) A bound provider SUBSTITUTES for the derivation: its claim
// reaches the quality gate even while the F02 mailboxes publish Good the
// whole time. One Insufficient provider axis alone must NOT gate (§10, a
// dead PPG must not condemn the IMU stream); BOTH axes Insufficient gates
// G2 immediately -> Unknown + LowSignalQuality.
void test_pump_provider_insufficient_gates_while_features_good(void) {
    PumpHarness h;
    h.bindProvider();

    // Single Insufficient axis (ppg): no gate — the engine still reaches
    // Awake from sustained calm (mirrors test_gates_single_insufficient_...),
    // and the provider's ppg claim shows in the snapshot while features
    // keep publishing Good.
    h.provider.active = true;
    h.provider.imu = bw::DataQuality::Good;
    h.provider.ppg = bw::DataQuality::Insufficient;
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs + 1000; t += 1000) {
        (void)h.tick(t);
    }
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));

    // BOTH provider axes Insufficient -> gate G2 on the next tick, features
    // still Good (without substitution the engine would stay Awake).
    h.provider.imu = bw::DataQuality::Insufficient;
    (void)h.tick(32000);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);
}

// (H10-2) Provider Good BEATS stalled F02: with the provider bound, the
// feature mailboxes stop publishing and — past kFeatureQualityStaleMs — the
// derivation channel would read Insufficient, but the provider keeps both
// channels Good and the engine stays Awake (substitution, full stop).
void test_pump_provider_good_beats_stalled_features(void) {
    PumpHarness h;
    h.bindProvider();
    h.provider.active = true;  // Good/Good every tick

    // Healthy pipeline to Awake (t <= 30000), running a little longer.
    for (bw::TimeMs t = 0; t <= 35000; t += 1000) {
        (void)h.tick(t);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));

    // F02 stops publishing BOTH feature channels (with_features=false);
    // the provider keeps returning Good past kFeatureQualityStaleMs.
    for (bw::TimeMs t = 36000; t <= 35000 + bw::state::kFeatureQualityStaleMs + 1000; t += 1000) {
        (void)h.tick(t, nullptr, false);
    }
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));
}

// (H10-3) Provider SILENCE (false returns) ages the channel back to
// Insufficient through the SAME kFeatureQualityStaleMs machinery —
// boundary inclusive: exactly at the limit the last provider value is
// still Good, one tick past it both axes are Insufficient and G2 gates —
// all of this while the feature mailboxes keep publishing Good (their
// receipts must not keep the provider's channel fresh: full substitution).
void test_pump_provider_silence_ages_then_gates(void) {
    PumpHarness h;
    h.bindProvider();
    h.provider.active = true;  // Good/Good every tick

    // Healthy pipeline to Awake; provider receipts stamped each tick.
    for (bw::TimeMs t = 0; t <= 35000; t += 1000) {
        (void)h.tick(t);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));

    // Provider goes silent at 35000 (last receipt); features stay healthy.
    h.provider.active = false;

    // Boundary: exactly kFeatureQualityStaleMs after the last receipt the
    // provider value is still Good and the engine still trusts it.
    (void)h.tick(35000 + bw::state::kFeatureQualityStaleMs);
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));

    // One pump tick past the limit: both provider channels aged to
    // Insufficient, gate G2 claims Unknown + LowSignalQuality.
    (void)h.tick(35000 + bw::state::kFeatureQualityStaleMs + 1000);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);
}

// (H10-4) A NEVER-RECEIVED provider (bound, always false) must not
// fabricate quality from the healthy F02 mailboxes: both axes stay
// Insufficient from the first tick and G2 gates — the derivation must NOT
// run while bound (without substitution the healthy features would drive
// the engine to Awake).
void test_pump_provider_never_received_insufficient_despite_healthy_features(void) {
    PumpHarness h;
    h.bindProvider();  // provider bound but active=false: never returned a value

    // First tick: features already publish Good, yet the channel is honest.
    (void)h.tick(0);
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));

    // Full calm window with healthy features: gated, never fabricated Awake.
    for (bw::TimeMs t = 1000; t <= bw::state::kAwakeRecoveryMs + 1000; t += 1000) {
        (void)h.tick(t);
    }
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);
}

// ---- FIX loop 1 (audit repair; H10 revert-proof) ----------------------------
// The recovery and mid-rung halves of the provider path, plus the defensive
// out-param seed. Each FAILS when the provider block in StatePump::tick() is
// disabled; the last one also FAILS when the Insufficient out-param seed is
// reverted to `q{}`.

// (FIX-1 + FIX-4) Recovery half of the provider path, never exercised before:
// after BOTH provider axes went Insufficient (gate G2), provider Good again
// must make the PROVIDER channel lift the gate — the feature mailboxes stay
// STALLED through the whole recovery, so the derivation channel reads
// Insufficient the whole time and could not be what recovers the engine — and
// the engine pays the FULL kAwakeRecoveryMs of calm back to Awake (exactly at
// it, never before). Afterwards a single Degraded provider axis discounts the
// snapshot confidence (D9) and sets the LowSignalQuality bit WITHOUT gating.
void test_pump_provider_good_recovers_after_gate_full_dwell(void) {
    PumpHarness h;
    h.bindProvider();
    h.provider.active = true;  // Good/Good every tick

    // Healthy pipeline to Awake (t <= kAwakeRecoveryMs).
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
        (void)h.tick(t);
    }
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));

    // Provider claims BOTH axes Insufficient; from here on the feature
    // mailboxes are STALLED (with_features=false), so ONLY the provider can
    // ever make the quality axis read Good again (full substitution, D16).
    h.provider.imu = bw::DataQuality::Insufficient;
    h.provider.ppg = bw::DataQuality::Insufficient;
    bw::state::StateOutputs out = h.tick(31000, nullptr, false);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);

    // Provider Good again: the gate falls this tick, but recovery must be
    // SUSTAINED (E3) — still Unknown with a fresh full dwell ahead.
    h.provider.imu = bw::DataQuality::Good;
    h.provider.ppg = bw::DataQuality::Good;
    (void)h.tick(32000, nullptr, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(h.last().alertness));

    // Boundary: ONE tick before kAwakeRecoveryMs of calm the engine still
    // holds Unknown (the full dwell is re-paid, never straddled), and the
    // snapshot already mirrors the provider's Good.
    (void)h.tick(32000 + bw::state::kAwakeRecoveryMs - 1000, nullptr, false);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));

    // Exactly kAwakeRecoveryMs after the gate fell: Awake regained on the
    // provider's Good, with the stalled derivation never asked.
    out = h.tick(32000 + bw::state::kAwakeRecoveryMs, nullptr, false);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_TRUE(out.transitioned);

    // Finding 4: ONE Degraded provider axis (ppg stays Good) — snapshot
    // confidence discount + reasons bit, NO gate (Degraded is not
    // Insufficient). Awake counts as a rung for D9, so confidence =
    // 0.8 (F06) * 0.9 (wear) * 0.75 (one degraded source) = 0.54.
    h.provider.imu = bw::DataQuality::Degraded;
    (void)h.tick(32000 + bw::state::kAwakeRecoveryMs + 1000, nullptr, false);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Degraded),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.54f, snap.confidence);
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::SustainedWakefulnessProbability) |
                                 f07_test::bitOf(bw::ReasonCode::LowSignalQuality),
                             snap.reasons);
}

// (FIX-3) The provider gate fired MID-RUNG: provider Good, drowsy evidence
// escalates Awake -> Drowsy through the harness mailbox, then BOTH provider
// axes Insufficient gate the engine mid-rung (Unknown + LowSignalQuality).
// When the provider returns Good again with the SAME drowsy evidence still
// publishing, Drowsy may only be re-claimed after the FULL rung dwell —
// pinning the D6 gate-RISING-edge evidence reset through the PROVIDER path
// (without the reset the old rung run would straddle the gate and Drowsy
// would return instantly).
void test_pump_provider_gate_mid_rung_requires_full_rung_dwell(void) {
    // Instant evidence landing (test-only config injection, kInstantEmaAlpha):
    // with the default alpha 0.2 the EMA was already seeded by the calm phase
    // and ramps toward the drowsy band over ~9 ticks — EMA arithmetic that
    // this test refuses to encode in expectations (§9 harness rule).
    bw::state::StateEngineConfig cfg;
    cfg.ema_alpha = f07test::kInstantEmaAlpha;
    PumpHarness h(cfg);
    h.bindProvider();
    h.provider.active = true;  // Good/Good every tick

    // Healthy pipeline to Awake (t <= kAwakeRecoveryMs).
    for (bw::TimeMs t = 0; t <= bw::state::kAwakeRecoveryMs; t += 1000) {
        (void)h.tick(t);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));

    // Drowsy-band evidence from 31000: the rung dwell runs exactly
    // kDrowsyMinDurationMs — Awake one tick before, Drowsy exactly at it.
    for (bw::TimeMs t = 31000; t < 31000 + bw::state::kDrowsyMinDurationMs; t += 1000) {
        const bw::AlertnessProbabilities a = f07test::drowsyAlertnessAt(t);
        (void)h.tick(t, nullptr, true, &a);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(h.last().alertness));
    bw::TimeMs t = 31000 + bw::state::kDrowsyMinDurationMs;
    bw::AlertnessProbabilities a = f07test::drowsyAlertnessAt(t);
    bw::state::StateOutputs out = h.tick(t, nullptr, true, &a);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(h.last().alertness));
    TEST_ASSERT_TRUE(out.transitioned);

    // Provider gate MID-RUNG: both axes Insufficient while the drowsy
    // evidence keeps publishing — Unknown + LowSignalQuality next tick.
    h.provider.imu = bw::DataQuality::Insufficient;
    h.provider.ppg = bw::DataQuality::Insufficient;
    t = 31000 + bw::state::kDrowsyMinDurationMs + 1000;
    a = f07test::drowsyAlertnessAt(t);
    out = h.tick(t, nullptr, true, &a);
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);

    // Provider Good again: the gate falls, but the rising-edge evidence
    // reset (D6) means the FULL rung dwell is re-paid — no instant re-claim,
    // Unknown one tick before the dwell completes, Drowsy exactly at it.
    h.provider.imu = bw::DataQuality::Good;
    h.provider.ppg = bw::DataQuality::Good;
    const bw::TimeMs gate_clear = 31000 + bw::state::kDrowsyMinDurationMs + 2000;
    t = gate_clear;
    a = f07test::drowsyAlertnessAt(t);
    (void)h.tick(t, nullptr, true, &a);  // gate falls this tick
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(h.last().alertness));
    t = gate_clear + bw::state::kDrowsyMinDurationMs - 1000;
    a = f07test::drowsyAlertnessAt(t);
    (void)h.tick(t, nullptr, true, &a);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(h.last().alertness));
    t = gate_clear + bw::state::kDrowsyMinDurationMs;
    a = f07test::drowsyAlertnessAt(t);
    out = h.tick(t, nullptr, true, &a);
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Good),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(out.previous));
}

// (FIX-2) A DEFECTIVE provider that returns true WITHOUT writing `out` must
// never fabricate healthy data: the pump seeds the out-param
// Insufficient/Insufficient, so the unwritten receipt reads "no reliable
// data" and gates (00 §1). Reverting the seed to `q{}` makes the zero bytes
// read Good/Good (DataQuality::Good == 0) and this test FAILS (the engine
// would reach Awake on fabricated quality).
void test_pump_provider_true_without_write_is_insufficient(void) {
    PumpHarness h;
    h.bindProvider();
    h.provider.active = true;
    h.provider.writes = false;  // returns true, never writes `out`

    // First tick: honest Insufficient, never the zero-initialized Good.
    (void)h.tick(0);
    bw::StateSnapshot snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));

    // Full calm window with healthy features: gated, never fabricated Awake.
    for (bw::TimeMs t = 1000; t <= bw::state::kAwakeRecoveryMs + 1000; t += 1000) {
        (void)h.tick(t);
    }
    snap = h.last();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(snap.quality.ppg));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(snap.alertness));
    TEST_ASSERT_EQUAL_UINT32(f07_test::bitOf(bw::ReasonCode::LowSignalQuality), snap.reasons);
}
