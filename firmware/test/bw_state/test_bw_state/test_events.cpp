// F07 §9 test_events.cpp — the transition records of D11/D12: emitted on
// entering Drowsy / HighDrowsinessRisk / NotWorn ONLY, every field copied
// from the transition snapshot, registry id 7, no seq/boot_id (F17 assigns).

#include <bw/state/state_config.h>
#include <bw/state/state_engine.h>
#include <unity.h>

#include "state_test_support.h"

using bw::AlertnessState;
using bw::EventType;
using bw::state::StateEngine;
using bw::state::StateEngineConfig;
using bw::state::StateInputs;
using bw::state::StateOutputs;
using f07_test::bitOf;
using f07_test::freshInputs;

namespace {

StateInputs atEvidence(bw::TimeMs t, float evidence) {
    StateInputs in = freshInputs(t);
    in.alertness.sleep = 0.1f;
    in.alertness.drowsy = evidence - 0.1f;
    return in;
}

}  // namespace

// (D12) Exactly one DrowsinessEvent when entering Drowsy: emitted at the
// transition step only, duration = how long the previous state held.
void test_events_drowsiness_on_drowsy_escalation(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    StateEngine e;
    for (bw::TimeMs t = 0; t < kD; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.65f));
        TEST_ASSERT_FALSE(out.event_valid);  // nothing during the dwell
    }
    const StateOutputs out = e.step(atEvidence(kD, 0.65f));
    TEST_ASSERT_TRUE(out.event_valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DrowsinessEvent),
                          static_cast<int>(out.event.type));
    TEST_ASSERT_EQUAL_UINT8(bw::state::kStateFeatureId, out.event.detector_id);
    TEST_ASSERT_EQUAL_UINT64(kD, out.event.t_start_ms);   // transition time
    TEST_ASSERT_EQUAL_UINT32(kD, out.event.duration_ms);  // Unknown held kD ms
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.event.alertness_at_start));
}

// (D12) Escalation Drowsy -> HighDrowsinessRisk: a second DrowsinessEvent
// whose payload records the previous AND entered states, duration = the
// Drowsy run (kHighRiskMinDurationMs - kDrowsyMinDurationMs).
void test_events_high_risk_payload_prev_and_new(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    constexpr bw::TimeMs kH = bw::state::kHighRiskMinDurationMs;
    StateEngine e;
    bool got_drowsy_record = false;
    bw::Event high{};
    bool got_high_record = false;
    for (bw::TimeMs t = 0; t <= kH + 1000; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.80f));
        if (!out.event_valid) {
            continue;
        }
        if (out.snapshot.alertness == AlertnessState::Drowsy) {
            got_drowsy_record = true;
        }
        if (out.snapshot.alertness == AlertnessState::HighDrowsinessRisk) {
            high = out.event;
            got_high_record = true;
        }
    }
    TEST_ASSERT_TRUE(got_drowsy_record);
    TEST_ASSERT_TRUE(got_high_record);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::Drowsy),
                            high.payload[bw::state::kStatePayloadPrevIndex]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::HighDrowsinessRisk),
                            high.payload[bw::state::kStatePayloadNewIndex]);
    TEST_ASSERT_EQUAL_UINT32(kH - kD, high.duration_ms);  // the Drowsy run
    TEST_ASSERT_EQUAL_UINT64(kH, high.t_start_ms);
}

// (D12) A held state emits ONE record, not one per step.
void test_events_single_record_per_escalation(void) {
    StateEngine e;
    uint32_t records = 0;
    for (bw::TimeMs t = 0; t <= 600000; t += 1000) {  // 10 min held Drowsy
        if (e.step(atEvidence(t, 0.65f)).event_valid) {
            ++records;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, records);
}

// (D12) De-escalation (E2) and the E3 recovery emit NO record — only
// escalations are recorded (Unknown is F23/F26's, PossibleSleep is F08's).
void test_events_no_record_on_deescalation_recovery_or_unknown(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    uint32_t total_records = 0;
    uint32_t records_after_drowsy = 0;
    bool past_drowsy_entry = false;
    for (bw::TimeMs t = 0; t <= 200000; t += 1000) {
        float evidence = 0.65f;
        if (t > 60000 && t <= 100000) {
            evidence = 0.39f;  // calm: Drowsy -> Awake after the recovery dwell
        } else if (t > 100000) {
            evidence = 0.50f;  // inside the calm/exit band: holds Awake (E4)
        }
        const StateOutputs out = e.step(atEvidence(t, evidence));
        if (out.event_valid) {
            ++total_records;
            TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DrowsinessEvent),
                                  static_cast<int>(out.event.type));
            if (past_drowsy_entry) {
                ++records_after_drowsy;
            }
        }
        if (t == 60000) {
            past_drowsy_entry = true;
        }
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake), static_cast<int>(e.state()));
    TEST_ASSERT_EQUAL_UINT32(1, total_records);         // the one Drowsy entry
    TEST_ASSERT_EQUAL_UINT32(0, records_after_drowsy);  // nothing after it
}

// (D12) Entering PossibleSleep transitions but records NOTHING: F08 owns the
// SleepEpisode record.
void test_events_no_record_for_possible_sleep(void) {
    StateEngine e;
    bool reached = false;
    for (bw::TimeMs t = 0; t <= 300000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.85f;
        const StateOutputs out = e.step(in);
        if (e.state() == AlertnessState::PossibleSleep) {
            reached = true;
            TEST_ASSERT_FALSE(out.event_valid);  // this very step emits nothing
        }
        if (out.event_valid) {
            TEST_ASSERT_TRUE(out.event.type == EventType::DrowsinessEvent);
        }
    }
    TEST_ASSERT_TRUE(reached);
}

// (D12) Entering NotWorn emits DeviceNotWorn with the state-change payload.
void test_events_devicenotworn_record(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t <= 9000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.valid = false;
        e.step(in);  // Unknown gate period, no records
    }
    StateInputs in = freshInputs(10000);
    in.wear.state = bw::WearState::NotWorn;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_TRUE(out.event_valid);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EventType::DeviceNotWorn),
                          static_cast<int>(out.event.type));
    TEST_ASSERT_EQUAL_UINT8(bw::state::kStateFeatureId, out.event.detector_id);
    TEST_ASSERT_EQUAL_UINT64(10000, out.event.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(10000, out.event.duration_ms);  // Unknown held 10 s
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::NotWorn),
                            out.event.alertness_at_start);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::Unknown),
                            out.event.payload[bw::state::kStatePayloadPrevIndex]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertnessState::NotWorn),
                            out.event.payload[bw::state::kStatePayloadNewIndex]);
}

// (D12) The record mirrors the transition snapshot: confidence, reasons,
// quality and context are copies — never recomputed.
void test_events_record_copies_transition_snapshot(void) {
    StateEngine e;
    StateInputs proto = freshInputs(0);
    proto.alertness.drowsy = 0.55f;
    proto.alertness.sleep = 0.1f;  // evidence 0.65: Drowsy enters at 60 s
    proto.alertness.confidence = 0.8f;
    proto.wear.confidence = 0.9f;
    proto.alertness.reasons = 1u << static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion);
    proto.quality.ppg = bw::DataQuality::Degraded;  // mirrored, not recomputed
    bw::Event record{};
    bool got = false;
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        StateInputs in = proto;
        in.t_ms = t;
        in.alertness.t_ms = t;
        in.wear.t_ms = t;
        in.activity.t_ms = t;
        const StateOutputs out = e.step(in);
        if (out.event_valid) {
            record = out.event;
            // The record IS the transition snapshot (D12), bit for bit:
            TEST_ASSERT_EQUAL_UINT64(out.snapshot.t_ms, record.t_start_ms);
            TEST_ASSERT_FLOAT_WITHIN(0.00001f, out.snapshot.confidence, record.confidence);
            TEST_ASSERT_EQUAL_UINT32(out.snapshot.reasons, record.reasons);
            TEST_ASSERT_EQUAL_UINT8(out.snapshot.quality.ppg, record.quality.ppg);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(out.snapshot.activity),
                                  static_cast<int>(record.context));
            got = true;
        }
    }
    TEST_ASSERT_TRUE(got);
    // 0.8 x 0.9 x 0.75 (one degraded source) = 0.54
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.54f, record.confidence);
    TEST_ASSERT_TRUE((record.reasons & bitOf(bw::ReasonCode::LowSignalQuality)) != 0);
    TEST_ASSERT_TRUE((record.reasons & bitOf(bw::ReasonCode::SustainedLowAlertnessProbability)) !=
                     0);
    TEST_ASSERT_TRUE((record.reasons & bitOf(bw::ReasonCode::SustainedLowMotion)) !=
                     0);  // D14 pass-through
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::ActivityContext::Sitting),
                          static_cast<int>(record.context));
}

// (D12/registry) Detector id and reserved bytes: detector_id == 7 (registry),
// payload[2..7] == 0, seq/boot_id/t_start_unix == 0 until F17 assigns them,
// schema_version stamped.
void test_events_registry_and_reserved_fields(void) {
    StateEngine e;
    StateOutputs last{};
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.65f));
        if (out.event_valid) {
            last = out;
        }
    }
    TEST_ASSERT_TRUE(last.event_valid);
    TEST_ASSERT_EQUAL_UINT8(7, last.event.detector_id);
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, last.event.schema_version);
    TEST_ASSERT_EQUAL_UINT32(0, last.event.seq);
    TEST_ASSERT_EQUAL_UINT32(0, last.event.boot_id);
    TEST_ASSERT_EQUAL_UINT32(0, last.event.t_start_unix);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::UserLabel::Unlabeled),
                          static_cast<int>(last.event.label));
    for (uint8_t i = bw::state::kStatePayloadNewIndex + 1; i < sizeof(last.event.payload); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, last.event.payload[i]);
    }
}
