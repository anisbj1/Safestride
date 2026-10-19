// F07 §9 test_snapshot.cpp — snapshot field mirrors (D10), confidence
// composition (D9), reason bits (D9/D14) and D13 input validation, plus the
// StateProvider accessor (02 §5).

#include <limits>

#include <bw/containers/latest_value.h>
#include <bw/state/state_config.h>
#include <bw/state/state_engine.h>
#include <unity.h>

#include "state_test_support.h"

using bw::AlertnessState;
using bw::state::StateEngine;
using bw::state::StateInputs;
using bw::state::StateOutputs;
using bw::state::StateProvider;
using f07_test::bitOf;
using f07_test::freshInputs;

namespace {

// Drives `e` to Drowsy: evidence 0.65 held for the full dwell (the proto is
// updated so the caller's follow-up steps keep feeding the same evidence).
void establishDrowsy(StateEngine& e, bw::state::StateInputs& proto) {
    proto.alertness.drowsy = 0.55f;
    proto.alertness.sleep = 0.1f;  // drowsy + sleep = 0.65
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        StateInputs in = proto;
        in.t_ms = t;
        in.alertness.t_ms = t;
        in.wear.t_ms = t;
        in.activity.t_ms = t;
        e.step(in);
    }
}

}  // namespace

void test_snapshot_motion_evidence_keeps_original_timestamp_and_gates(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    establishDrowsy(e, proto);
    auto in = freshInputs(61000);
    in.alertness.wake_motion = {60500, true, true};
    auto out = e.step(in);
    TEST_ASSERT_TRUE(out.snapshot.wake_motion.valid);
    TEST_ASSERT_TRUE(out.snapshot.wake_motion.qualifying);
    TEST_ASSERT_EQUAL_UINT64(60500, out.snapshot.wake_motion.t_ms);
    in = freshInputs(62000);
    in.alertness.wake_motion = {60500, true, true};
    out = e.step(in);
    TEST_ASSERT_EQUAL_UINT64(60500, out.snapshot.wake_motion.t_ms);
    in.wear.state = bw::WearState::NotWorn;
    TEST_ASSERT_FALSE(e.step(in).snapshot.wake_motion.valid);
    in.wear.state = bw::WearState::Uncertain;
    TEST_ASSERT_FALSE(e.step(in).snapshot.wake_motion.valid);
    in.wear.state = bw::WearState::Worn;
    in.alertness.valid = false;
    TEST_ASSERT_FALSE(e.step(in).snapshot.wake_motion.valid);
}

// (D9) Unknown confidence is 0 — never fabricated (00 §1).
void test_snapshot_unknown_zero_confidence(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.alertness.valid = false;  // Unknown + SensorUnavailable
    const StateOutputs out = e.step(in);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, out.snapshot.confidence);
}

// (D9) NotWorn confidence = the wear confidence (clamped to [0,1]).
void test_snapshot_notworn_confidence_is_wear(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.wear.state = bw::WearState::NotWorn;
    in.wear.confidence = 0.42f;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.42f, out.snapshot.confidence);
}

// (D9) Ladder confidence = clamp01(alertness x wear).
void test_snapshot_ladder_confidence_is_product(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.confidence = 0.8f;
    proto.wear.confidence = 0.9f;
    establishDrowsy(e, proto);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    StateInputs in = proto;
    in.t_ms = 61000;
    in.alertness.t_ms = 61000;
    in.wear.t_ms = 61000;
    in.activity.t_ms = 61000;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.72f, out.snapshot.confidence);  // 0.8 x 0.9
}

// (D9) One Degraded source: confidence x kDegradedConfidenceFactor, and the
// LowSignalQuality bit is added — Degraded lowers confidence AND explains
// itself; it never becomes Unknown.
void test_snapshot_degraded_source_penalizes_and_explains(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.confidence = 0.8f;
    proto.wear.confidence = 0.9f;
    establishDrowsy(e, proto);
    StateInputs in = proto;
    in.t_ms = 61000;
    in.alertness.t_ms = 61000;
    in.wear.t_ms = 61000;
    in.activity.t_ms = 61000;
    in.quality.imu = bw::DataQuality::Degraded;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.snapshot.alertness));
    // 0.8 x 0.9 x 0.75 = 0.54
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.54f, out.snapshot.confidence);
    TEST_ASSERT_TRUE((out.snapshot.reasons & bitOf(bw::ReasonCode::LowSignalQuality)) != 0);
    TEST_ASSERT_TRUE(
        (out.snapshot.reasons & bitOf(bw::ReasonCode::SustainedLowAlertnessProbability)) != 0);
    TEST_ASSERT_EQUAL_UINT32(bw::DataQuality::Degraded, out.snapshot.quality.imu);
}

// (D9) Two degraded sources: factor squared (0.72 x 0.5625 = 0.405).
void test_snapshot_two_degraded_sources_squared(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.confidence = 0.8f;
    proto.wear.confidence = 0.9f;
    establishDrowsy(e, proto);
    StateInputs in = proto;
    in.t_ms = 61000;
    in.alertness.t_ms = 61000;
    in.wear.t_ms = 61000;
    in.activity.t_ms = 61000;
    in.quality.imu = bw::DataQuality::Degraded;
    in.quality.ppg = bw::DataQuality::Degraded;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.405f, out.snapshot.confidence);
}

// (D9) The composed confidence is clamped to [0,1] no matter the product.
void test_snapshot_confidence_clamped_to_one(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.confidence = 1.7f;  // out-of-range confidence (not a probability)
    proto.wear.confidence = 1.2f;
    establishDrowsy(e, proto);
    StateInputs in = proto;
    in.t_ms = 61000;
    in.alertness.t_ms = 61000;
    in.wear.t_ms = 61000;
    in.activity.t_ms = 61000;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_TRUE(out.snapshot.confidence <= 1.0f);
    TEST_ASSERT_TRUE(out.snapshot.confidence > 0.0f);
}

// (D14) F06's reason bits pass through into LADDER snapshots together with
// F07's own justification bit.
void test_snapshot_d14_reasons_pass_through_on_ladder(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.reasons = 1u << static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion);
    establishDrowsy(e, proto);
    StateInputs in = proto;
    in.t_ms = 61000;
    in.alertness.t_ms = 61000;
    in.wear.t_ms = 61000;
    in.activity.t_ms = 61000;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.snapshot.alertness));
    TEST_ASSERT_TRUE(
        (out.snapshot.reasons & bitOf(bw::ReasonCode::SustainedLowAlertnessProbability)) != 0);
    TEST_ASSERT_TRUE((out.snapshot.reasons & bitOf(bw::ReasonCode::SustainedLowMotion)) !=
                     0);  // F06's bit
}

// (D14) Gate states NEVER inherit F06's bits: Unknown from E4 carries only
// InsufficientEvidence even though the alertness input carries reason bits.
void test_snapshot_d14_no_pass_through_on_unknown(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.alertness.drowsy = 0.4f;
    in.alertness.sleep = 0.1f;  // evidence 0.5: ambiguous-but-stable -> E4
    in.alertness.reasons = 1u << static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion);
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(out.snapshot.alertness));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
}

// (D14/D13) No pass-through on a rejected input: valid=false with reason bits
// set yields exactly SensorUnavailable — the stale/fault bits are F07's call.
void test_snapshot_d14_no_pass_through_on_rejected_input(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.alertness.valid = false;
    in.alertness.reasons = 1u << static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion);
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(out.snapshot.alertness));
    TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
}

// (D13) Probabilities must be finite and inside [0,1]: NaN, 1.5 and -0.1 all
// gate to Unknown + SensorUnavailable — never clamped into a plausible claim.
void test_snapshot_invalid_probabilities_gate(void) {
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), 1.5f, -0.1f,
                         std::numeric_limits<float>::infinity()};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        StateEngine e;
        StateInputs in = freshInputs(10000);
        in.alertness.drowsy = bad[i];
        const StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::SensorUnavailable), out.snapshot.reasons);
        TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, out.snapshot.confidence);
    }
    {  // NaN confidence feeds the D9 product -> same rejection
        StateEngine e;
        StateInputs in = freshInputs(10000);
        in.alertness.confidence = std::numeric_limits<float>::quiet_NaN();
        const StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
    }
}

// (D10) mode/power/quality mirror the inputs; a STALE activity axis mirrors
// Unknown (it describes the past), a stale wear axis mirrors Uncertain.
void test_snapshot_mirrors_axes_and_stale_axes_collapse(void) {
    StateEngine e;
    StateInputs in = freshInputs(10000);
    in.mode = bw::OperatingMode::Night;
    in.power = bw::PowerProfile::LowPower;
    in.activity.t_ms = 10000 - bw::state::kInputStaleMs - 1;  // stale
    in.wear.t_ms = 10000 - bw::state::kInputStaleMs - 1;      // stale
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::OperatingMode::Night),
                          static_cast<int>(out.snapshot.mode));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::PowerProfile::LowPower),
                          static_cast<int>(out.snapshot.power));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ActivityContext::Unknown),
                          static_cast<int>(out.snapshot.activity));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::WearState::Uncertain),
                          static_cast<int>(out.snapshot.wear));
}

// (D10) in_state_ms accumulates while held and resets on every transition.
void test_snapshot_in_state_ms_accumulates_and_resets(void) {
    StateEngine e;
    StateInputs in = freshInputs(0);
    in.alertness.valid = false;
    for (bw::TimeMs t = 0; t <= 5000; t += 1000) {
        in.t_ms = t;
        in.alertness.t_ms = t;
        in.wear.t_ms = t;
        in.activity.t_ms = t;
        const StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(t), out.snapshot.in_state_ms);
    }
    // Gate clears: the Unknown hold keeps accumulating across ticks ...
    for (bw::TimeMs t = 6000; t < 6000 + bw::state::kAwakeRecoveryMs; t += 1000) {
        const StateOutputs out = e.step(freshInputs(t));
        TEST_ASSERT_FALSE(out.transitioned);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(t), out.snapshot.in_state_ms);
    }
    // ... then the sustained-calm transition resets the clock to 0.
    const StateOutputs out = e.step(freshInputs(6000 + bw::state::kAwakeRecoveryMs));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(out.snapshot.alertness));
    TEST_ASSERT_EQUAL_UINT32(0, out.snapshot.in_state_ms);
}

// (02 §5) StateProvider reads the newest snapshot from the bound mailbox and
// answers false when nothing is bound or nothing was published yet.
void test_snapshot_state_provider(void) {
    StateProvider unbound;
    bw::StateSnapshot s{};
    TEST_ASSERT_FALSE(unbound.latest(s));

    bw::LatestValue<bw::StateSnapshot> mailbox;
    StateProvider p;
    p.bind(&mailbox);
    TEST_ASSERT_FALSE(p.latest(s));  // bound, but empty

    bw::StateSnapshot first{};
    first.t_ms = 111;
    first.alertness = AlertnessState::Awake;
    mailbox.set(first);
    bw::StateSnapshot second{};
    TEST_ASSERT_TRUE(p.latest(second));
    TEST_ASSERT_EQUAL_UINT64(111, second.t_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                          static_cast<int>(second.alertness));
}

// (§11 budget) Static sizes: engine below 1 KB, outputs below 512 B — the
// pump keeps them on the task stack (no heap after startup).
void test_snapshot_sizes_within_budget(void) {
    TEST_ASSERT_TRUE(sizeof(StateEngine) <= 1024);
    TEST_ASSERT_TRUE(sizeof(StateOutputs) <= 512);
}
