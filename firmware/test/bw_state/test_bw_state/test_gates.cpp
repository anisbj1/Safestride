// F07 §9 test_gates.cpp — gates G1/G2/G3a + the E3 gate-exit rule, one case
// per row of the normative table (spec §7). Time is data (D4): each test is a
// (t_ms, inputs) table driving the engine directly.

#include <bw/state/state_config.h>
#include <bw/state/state_engine.h>
#include <unity.h>

#include "state_test_support.h"

using bw::AlertnessState;
using bw::state::StateEngine;
using bw::state::StateInputs;
using bw::state::StateOutputs;
using f07_test::bitOf;
using f07_test::freshInputs;

void test_gates_uncertain_wear_cannot_advance_to_awake() {
    StateEngine engine;
    // Even sustained high awake probability and Good quality cannot turn
    // unconfirmed startup wear into a physiological state.
    for (bw::TimeMs time = 0; time <= 60000; time += 1000) {
        StateInputs in = freshInputs(time);
        in.wear.state = bw::WearState::Uncertain;
        in.alertness.awake = 1.0f;
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.0f;
        const StateOutputs out = engine.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out.snapshot.confidence);
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
    }
}

// (G1/G2) Boot with no data at all: the honest answer is Unknown +
// SensorUnavailable, confidence 0 — never a fabricated Awake (00 §1).
void test_gates_boot_no_input_is_unknown_sensor_unavailable(void) {
    StateEngine e;
    StateInputs in{};  // never-received contract defaults: valid=false
    in.t_ms = 0;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_FALSE(out.transitioned);  // construction state IS Unknown
    TEST_ASSERT_FALSE(out.event_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, out.snapshot.confidence);
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
}

// (G1) NotWorn wins over an Insufficient alertness stream: the fixed gate
// priority is NotWorn > Insufficient data > ladder (D3).
void test_gates_notworn_beats_insufficient_quality(void) {
    StateEngine e;
    StateInputs in = freshInputs(1000);
    in.wear.state = bw::WearState::NotWorn;
    in.alertness.input_quality = bw::DataQuality::Insufficient;  // would gate G2
    in.quality.ppg = bw::DataQuality::Insufficient;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::NotWorn), static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(out.previous));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.9f, out.snapshot.confidence);  // wear.confidence
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::DeviceNotWorn), out.snapshot.reasons);
}

// (D3) A gate is only asserted on FRESH evidence: a stale NotWorn estimate
// may not claim NotWorn — it falls to gate Unknown.
void test_gates_stale_wear_cannot_claim_not_worn(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.wear.t_ms = 10000 - bw::state::kInputStaleMs - 1;  // one ms too old
    in.wear.state = bw::WearState::NotWorn;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
}

// (G2) Staleness boundary: exactly kInputStaleMs counts as fresh (gate clear,
// so the Unknown reason becomes InsufficientEvidence, not a sensor claim);
// one ms more gates to Unknown + SensorUnavailable.
void test_gates_stale_boundary_is_exactly_input_stale_ms(void) {
    {
        StateEngine e;
        StateInputs in = freshInputs(10000);
        in.activity.t_ms = 10000 - bw::state::kInputStaleMs;  // exactly at the limit
        StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
    }
    {
        StateEngine e;
        StateInputs in = freshInputs(10000);
        in.activity.t_ms = 10000 - bw::state::kInputStaleMs - 1;  // one ms stale
        StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(e.state()));
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
    }
}

// (G2) valid == false gates the same as missing data (D5, §10).
void test_gates_valid_false_gates(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.alertness.valid = false;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
}

// (G2) Alertness staleness has its own limit (D5): exactly
// kAlertnessStaleMs is tolerated, one ms more gates.
void test_gates_alertness_stale_boundary(void) {
    {
        StateEngine e;
        StateInputs in = freshInputs(100000);
        in.alertness.t_ms = 100000 - bw::state::kAlertnessStaleMs;  // one late epoch
        StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
    }
    {
        StateEngine e;
        StateInputs in = freshInputs(100000);
        in.alertness.t_ms = 100000 - bw::state::kAlertnessStaleMs - 1;  // two epochs
        StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
    }
}

// (§10) ONE SourceQuality source marked Insufficient does not gate on its
// own: a dead PPG must not condemn the IMU stream.
void test_gates_single_insufficient_source_does_not_gate(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.quality.imu = bw::DataQuality::Insufficient;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
}

// (§10) BOTH sources Insufficient gates: Unknown + LowSignalQuality.
void test_gates_both_sources_insufficient_gates(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.quality.imu = bw::DataQuality::Insufficient;
    in.quality.ppg = bw::DataQuality::Insufficient;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::LowSignalQuality), out.snapshot.reasons);
}

// (§10) alertness.input_quality == Insufficient gates on its own — the
// alertness side decides through its own quality (D5).
void test_gates_alertness_insufficient_quality_gates(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.alertness.input_quality = bw::DataQuality::Insufficient;
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::LowSignalQuality), out.snapshot.reasons);
}

// (G3a + E3) NotWorn -> Worn goes to Unknown FIRST, never straight to Awake
// (D6); recovery then needs the FULL sustained calm: Unknown before the first
// full recovery interval after gate entry, Awake exactly at kAwakeRecoveryMs.
void test_gates_g3a_unknown_first_then_sustained_calm(void) {
    StateEngine e;
    StateInputs in = freshInputs(0);
    in.wear.state = bw::WearState::NotWorn;
    e.step(in);

    in = freshInputs(10000);  // Worn again: G1 clears, G3a fires
    StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::NotWorn),
                          static_cast<int>(out.previous));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);

    in = freshInputs(10000 + bw::state::kAwakeRecoveryMs - 1);  // one ms short
    e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));

    in = freshInputs(10000 + bw::state::kAwakeRecoveryMs);  // exactly the dwell
    out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake), static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
}

void test_gates_unknown_recovers_one_second_after_gates_clear(void) {
    StateEngine e;
    StateInputs in = freshInputs(0);
    in.alertness.valid = false;
    (void)e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));

    // The gate clears at 1 s; recovery starts there and needs one full
    // 1 Hz interval, not immediate Awake on the first usable sample.
    (void)e.step(freshInputs(1000));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    (void)e.step(freshInputs(1999));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    const StateOutputs out = e.step(freshInputs(2000));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(out.snapshot.alertness));
}

// (G2 mid-Unknown blip + E3, D6) A gate asserted while the engine is ALREADY
// Unknown (a one-tick both-sources-Insufficient blip) must still reset the
// evidence timers: the recovery dwell may not straddle a window the engine
// itself declared unusable (D6 "timers reset at gate entry"; §7 G2 side
// effect "reset" — auditor A-01). Calm from boot-Unknown -> 1 tick gate ->
// restore: the engine stays Unknown until a FRESH full kAwakeRecoveryMs of
// calm. This is also the §9 "quality restored + calm" recovery-timing case
// (auditor A-02): Unknown one tick before the recovery boundary, Awake exactly
// at kAwakeRecoveryMs.
void test_gates_quality_blip_while_unknown_resets_recovery_dwell(void) {
    StateEngine e;
    // t = 0..29000: healthy but ambiguous evidence keeps boot-Unknown without
    // triggering the shortened calm recovery dwell.
    for (bw::TimeMs t = 0; t <= 29000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.5f;
        in.alertness.sleep = 0.0f;
        e.step(in);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));

    // One tick of G2 (both sources Insufficient): the state does NOT change
    // (already Unknown), but the gate is asserted.
    StateInputs blip = freshInputs(30000);
    blip.quality.imu = bw::DataQuality::Insufficient;
    blip.quality.ppg = bw::DataQuality::Insufficient;
    e.step(blip);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));

    // Restored: the calm counter must have been ZEROED at the gate rise, so
    // every fresh calm tick is still Unknown up to the one-ms-short boundary
    for (bw::TimeMs t = 31000; t <= 31000 + bw::state::kAwakeRecoveryMs - 1000; t += 1000) {
        StateOutputs out = e.step(freshInputs(t));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(e.state()));
        TEST_ASSERT_FALSE(out.transitioned);
    }
    // ... and Awake exactly one full kAwakeRecoveryMs of calm after the gate
    // cleared (with the pre-fix freeze the engine would already be Awake at
    // t = 31000, so this test fails if the rising-edge reset is reverted).
    StateOutputs out = e.step(freshInputs(31000 + bw::state::kAwakeRecoveryMs));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake), static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
}

// (G1, flapping) Fresh NotWorn <-> Worn flapping: the engine FOLLOWS the gate
// immediately in both directions (enter fast, leave fast — the table's G1/G3a
// are both instantaneous; the anti-flapping dwells live on the LADDER rungs,
// not on the gates). Corrects the §9 row's "stays NotWorn" wording — a gate
// may never sit on stale evidence (status log 2026-10-11).
void test_gates_flapping_notworn_worn_follows_gate(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t < 24000; t += 3000) {
        StateInputs in = freshInputs(t);
        in.wear.state = (t / 3000) % 2 == 0 ? bw::WearState::NotWorn : bw::WearState::Worn;
        const StateOutputs out = e.step(in);
        const AlertnessState expected =
            (t / 3000) % 2 == 0 ? AlertnessState::NotWorn : AlertnessState::Unknown;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(e.state()));
        // The ladder never engages during the flap.
        TEST_ASSERT_TRUE(out.snapshot.alertness == AlertnessState::NotWorn ||
                         out.snapshot.alertness == AlertnessState::Unknown);
    }
}

// (flapping, corrected row) Wear flapping Worn <-> Uncertain — the realistic
// F04 output under a flapping raw signal — never claims NotWorn, settles in
// Unknown until a continuous run of confirmed wear clears recovery dwell.
void test_gates_flapping_worn_uncertain_bounded_transitions(void) {
    StateEngine e;
    uint32_t transitions = 0;
    bool reached_awake = false;
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.wear.state = (t / 1000) % 2 == 0 ? bw::WearState::Worn : bw::WearState::Uncertain;
        const StateOutputs out = e.step(in);
        if (out.transitioned) {
            ++transitions;
        }
        TEST_ASSERT_TRUE(out.snapshot.alertness != AlertnessState::NotWorn);
        if (e.state() == AlertnessState::Awake) {
            reached_awake = true;
        }
    }
    // Uncertain now gates inference: intermittent Worn cannot accumulate
    // evidence across unconfirmed wear and claim Awake (startup regression).
    TEST_ASSERT_FALSE(reached_awake);
    TEST_ASSERT_EQUAL_UINT32(0, transitions);
}
