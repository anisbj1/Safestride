#include <bw/core.h>

#include <unity.h>

#include <cstddef>
#include <cstdio>
#include <type_traits>

namespace {

// ---- compile-time layout pins (D3 layer a lives in bw/contract_checks.h; this
// file repeats them so the TEST output names the offender even if a feature
// includes individual headers instead of the umbrella) --------------------------
#define PIN_CONTRACT(Type, Bytes, Align)                                                           \
    static_assert(std::is_trivially_copyable_v<Type>, #Type ": not trivially copyable");           \
    static_assert(sizeof(Type) == (Bytes), #Type ": sizeof drifted");                              \
    static_assert(alignof(Type) == (Align), #Type ": alignof drifted")

PIN_CONTRACT(bw::ImuSample, 32, 8);
PIN_CONTRACT(bw::PpgSample, 24, 8);
PIN_CONTRACT(bw::Rgb, 3, 1);
PIN_CONTRACT(bw::BatteryReading, 16, 4);
PIN_CONTRACT(bw::SourceQuality, 2, 1);
PIN_CONTRACT(bw::MotionFeatures, 40, 8);
PIN_CONTRACT(bw::PpgFeatures, 40, 8);
PIN_CONTRACT(bw::ActivityEstimate, 16, 8);
PIN_CONTRACT(bw::WearEstimate, 16, 8);
PIN_CONTRACT(bw::FeatureVector, 144, 8);
PIN_CONTRACT(bw::ModelOutput, 40, 4);
PIN_CONTRACT(bw::Contribution, 8, 4);
PIN_CONTRACT(bw::WakeMotionEvidence, 16, 8);      // CR-034, timestamp + two flags
PIN_CONTRACT(bw::AlertnessProbabilities, 56, 8);  // CR-034, appended evidence
PIN_CONTRACT(bw::StateSnapshot, 48, 8);           // CR-034, appended evidence
PIN_CONTRACT(bw::Event, 48, 8);                   // kEventBytes, CR-002
PIN_CONTRACT(bw::AlertStatus, 24, 8);
PIN_CONTRACT(bw::ButtonEvent, 16, 8);
PIN_CONTRACT(bw::Command, 32, 8);
PIN_CONTRACT(bw::BaselineProfile, 20, 4);
PIN_CONTRACT(bw::HealthSnapshot, 32, 8);
PIN_CONTRACT(bw::SettingValue, 8, 4);
PIN_CONTRACT(bw::TaskHealth, 24, 8);
static_assert(sizeof(bw::Event) == bw::kEventBytes, "sizeof(Event) must equal kEventBytes");
#undef PIN_CONTRACT

}  // namespace

void test_contract_layout_sizes_and_alignment(void) {
    // Already enforced at compile time above; this keeps the suite reporting a named
    // PASS/FAIL row for reviewers who only read test output.
    TEST_ASSERT_EQUAL_UINT32(48, sizeof(bw::Event));
    TEST_ASSERT_EQUAL_UINT32(48, bw::kEventBytes);
    TEST_ASSERT_EQUAL_UINT32(8, alignof(bw::Event));
    TEST_ASSERT_EQUAL_UINT32(1, bw::Event::kSchemaVersion);
    TEST_PASS_MESSAGE("contract struct sizes/alignment pinned");
}

void test_contract_enum_values_append_only(void) {
    struct Row {
        unsigned actual;
        unsigned expected;
        const char* name;
    };

    const Row rows[] = {
        // AlertnessState (02 §4.5)
        {static_cast<unsigned>(bw::AlertnessState::Unknown), 0, "AlertnessState::Unknown"},
        {static_cast<unsigned>(bw::AlertnessState::NotWorn), 1, "AlertnessState::NotWorn"},
        {static_cast<unsigned>(bw::AlertnessState::Awake), 2, "AlertnessState::Awake"},
        {static_cast<unsigned>(bw::AlertnessState::Drowsy), 3, "AlertnessState::Drowsy"},
        {static_cast<unsigned>(bw::AlertnessState::HighDrowsinessRisk), 4,
         "AlertnessState::HighDrowsinessRisk"},
        {static_cast<unsigned>(bw::AlertnessState::PossibleSleep), 5,
         "AlertnessState::PossibleSleep"},
        // WearState (02 §4.3)
        {static_cast<unsigned>(bw::WearState::Uncertain), 0, "WearState::Uncertain"},
        {static_cast<unsigned>(bw::WearState::Worn), 1, "WearState::Worn"},
        {static_cast<unsigned>(bw::WearState::NotWorn), 2, "WearState::NotWorn"},
        // ActivityContext (02 §4.3)
        {static_cast<unsigned>(bw::ActivityContext::Unknown), 0, "ActivityContext::Unknown"},
        {static_cast<unsigned>(bw::ActivityContext::Stationary), 1, "ActivityContext::Stationary"},
        {static_cast<unsigned>(bw::ActivityContext::Sitting), 2, "ActivityContext::Sitting"},
        {static_cast<unsigned>(bw::ActivityContext::Standing), 3, "ActivityContext::Standing"},
        {static_cast<unsigned>(bw::ActivityContext::Walking), 4, "ActivityContext::Walking"},
        {static_cast<unsigned>(bw::ActivityContext::Running), 5, "ActivityContext::Running"},
        {static_cast<unsigned>(bw::ActivityContext::LyingResting), 6,
         "ActivityContext::LyingResting"},
        {static_cast<unsigned>(bw::ActivityContext::InactiveProlonged), 7,
         "ActivityContext::InactiveProlonged"},
        // DataQuality (02 §4.1)
        {static_cast<unsigned>(bw::DataQuality::Good), 0, "DataQuality::Good"},
        {static_cast<unsigned>(bw::DataQuality::Degraded), 1, "DataQuality::Degraded"},
        {static_cast<unsigned>(bw::DataQuality::Insufficient), 2, "DataQuality::Insufficient"},
        // OperatingMode / PowerProfile (02 §4.5)
        {static_cast<unsigned>(bw::OperatingMode::Day), 0, "OperatingMode::Day"},
        {static_cast<unsigned>(bw::OperatingMode::Night), 1, "OperatingMode::Night"},
        {static_cast<unsigned>(bw::OperatingMode::Nap), 2, "OperatingMode::Nap"},
        {static_cast<unsigned>(bw::OperatingMode::Calibration), 3, "OperatingMode::Calibration"},
        {static_cast<unsigned>(bw::OperatingMode::Research), 4, "OperatingMode::Research"},
        {static_cast<unsigned>(bw::PowerProfile::Normal), 0, "PowerProfile::Normal"},
        {static_cast<unsigned>(bw::PowerProfile::LowPower), 1, "PowerProfile::LowPower"},
        // AlertLevel (02 §4.6)
        {static_cast<unsigned>(bw::AlertLevel::Normal), 0, "AlertLevel::Normal"},
        {static_cast<unsigned>(bw::AlertLevel::Warning), 1, "AlertLevel::Warning"},
        {static_cast<unsigned>(bw::AlertLevel::Active), 2, "AlertLevel::Active"},
        {static_cast<unsigned>(bw::AlertLevel::Persistent), 3, "AlertLevel::Persistent"},
        // ButtonGesture + ButtonId (02 §4.6/§3)
        {static_cast<unsigned>(bw::ButtonGesture::ShortPress), 0, "ButtonGesture::ShortPress"},
        {static_cast<unsigned>(bw::ButtonGesture::LongPress), 1, "ButtonGesture::LongPress"},
        {static_cast<unsigned>(bw::ButtonGesture::DoublePress), 2, "ButtonGesture::DoublePress"},
        {static_cast<unsigned>(bw::ButtonId::Big), 0, "ButtonId::Big"},
        {static_cast<unsigned>(bw::ButtonId::Small), 1, "ButtonId::Small"},
        // EventType (02 §4.5)
        {static_cast<unsigned>(bw::EventType::SleepEpisode), 0, "EventType::SleepEpisode"},
        {static_cast<unsigned>(bw::EventType::DrowsinessEvent), 1, "EventType::DrowsinessEvent"},
        {static_cast<unsigned>(bw::EventType::SuspectedCataplexy), 2,
         "EventType::SuspectedCataplexy"},
        {static_cast<unsigned>(bw::EventType::UserMarked), 3, "EventType::UserMarked"},
        {static_cast<unsigned>(bw::EventType::AlertRaised), 4, "EventType::AlertRaised"},
        {static_cast<unsigned>(bw::EventType::SensorFault), 5, "EventType::SensorFault"},
        {static_cast<unsigned>(bw::EventType::DeviceNotWorn), 6, "EventType::DeviceNotWorn"},
        {static_cast<unsigned>(bw::EventType::BatteryWarning), 7, "EventType::BatteryWarning"},
        {static_cast<unsigned>(bw::EventType::NapPlanned), 8, "EventType::NapPlanned"},
        {static_cast<unsigned>(bw::EventType::NapCompleted), 9, "EventType::NapCompleted"},
        {static_cast<unsigned>(bw::EventType::TimeSync), 10, "EventType::TimeSync"},
        {static_cast<unsigned>(bw::EventType::Boot), 11, "EventType::Boot"},
        // UserLabel (02 §4.5)
        {static_cast<unsigned>(bw::UserLabel::Unlabeled), 0, "UserLabel::Unlabeled"},
        {static_cast<unsigned>(bw::UserLabel::Confirmed), 1, "UserLabel::Confirmed"},
        {static_cast<unsigned>(bw::UserLabel::Rejected), 2, "UserLabel::Rejected"},
        // ReasonCode (02 §4.5)
        {static_cast<unsigned>(bw::ReasonCode::SustainedLowMotion), 0,
         "ReasonCode::SustainedLowMotion"},
        {static_cast<unsigned>(bw::ReasonCode::ProlongedInactivity), 1,
         "ReasonCode::ProlongedInactivity"},
        {static_cast<unsigned>(bw::ReasonCode::HrBelowBaseline), 2, "ReasonCode::HrBelowBaseline"},
        {static_cast<unsigned>(bw::ReasonCode::HrTrendChange), 3, "ReasonCode::HrTrendChange"},
        {static_cast<unsigned>(bw::ReasonCode::AbruptMotionDrop), 4,
         "ReasonCode::AbruptMotionDrop"},
        {static_cast<unsigned>(bw::ReasonCode::TimeOfDayTypicalDrowsy), 5,
         "ReasonCode::TimeOfDayTypicalDrowsy"},
        {static_cast<unsigned>(bw::ReasonCode::MatchesPastConfirmedEvent), 6,
         "ReasonCode::MatchesPastConfirmedEvent"},
        {static_cast<unsigned>(bw::ReasonCode::UserMarked), 7, "ReasonCode::UserMarked"},
        {static_cast<unsigned>(bw::ReasonCode::PlannedNap), 8, "ReasonCode::PlannedNap"},
        {static_cast<unsigned>(bw::ReasonCode::LowSignalQuality), 9,
         "ReasonCode::LowSignalQuality"},
        {static_cast<unsigned>(bw::ReasonCode::SensorUnavailable), 10,
         "ReasonCode::SensorUnavailable"},
        {static_cast<unsigned>(bw::ReasonCode::DeviceNotWorn), 11, "ReasonCode::DeviceNotWorn"},
        // CR-008 (accepted 2026-10-11): state-engine justification codes, bits 12–14.
        {static_cast<unsigned>(bw::ReasonCode::SustainedLowAlertnessProbability), 12,
         "ReasonCode::SustainedLowAlertnessProbability"},
        {static_cast<unsigned>(bw::ReasonCode::SustainedWakefulnessProbability), 13,
         "ReasonCode::SustainedWakefulnessProbability"},
        {static_cast<unsigned>(bw::ReasonCode::InsufficientEvidence), 14,
         "ReasonCode::InsufficientEvidence"},
        // CR-009 (accepted 2026-10-11; landed after CR-008 per the G0 ordering
        // decision): F09 confounder diagnostics, bits 15–17.
        {static_cast<unsigned>(bw::ReasonCode::PossibleFall), 15, "ReasonCode::PossibleFall"},
        {static_cast<unsigned>(bw::ReasonCode::PostureChange), 16, "ReasonCode::PostureChange"},
        {static_cast<unsigned>(bw::ReasonCode::ShortMotionBurst), 17,
         "ReasonCode::ShortMotionBurst"},
        // CommandType (02 §4.6)
        {static_cast<unsigned>(bw::CommandType::AcknowledgeAlert), 0,
         "CommandType::AcknowledgeAlert"},
        {static_cast<unsigned>(bw::CommandType::CancelAlert), 1, "CommandType::CancelAlert"},
        {static_cast<unsigned>(bw::CommandType::NextScreen), 2, "CommandType::NextScreen"},
        {static_cast<unsigned>(bw::CommandType::OpenMenu), 3, "CommandType::OpenMenu"},
        {static_cast<unsigned>(bw::CommandType::MarkEvent), 4, "CommandType::MarkEvent"},
        {static_cast<unsigned>(bw::CommandType::ConfirmEvent), 5, "CommandType::ConfirmEvent"},
        {static_cast<unsigned>(bw::CommandType::RejectEvent), 6, "CommandType::RejectEvent"},
        {static_cast<unsigned>(bw::CommandType::StartNap), 7, "CommandType::StartNap"},
        {static_cast<unsigned>(bw::CommandType::CancelNap), 8, "CommandType::CancelNap"},
        {static_cast<unsigned>(bw::CommandType::SetMode), 9, "CommandType::SetMode"},
        {static_cast<unsigned>(bw::CommandType::SetSetting), 10, "CommandType::SetSetting"},
        {static_cast<unsigned>(bw::CommandType::SetTime), 11, "CommandType::SetTime"},
        {static_cast<unsigned>(bw::CommandType::StartPairing), 12, "CommandType::StartPairing"},
        {static_cast<unsigned>(bw::CommandType::EnterResearch), 13, "CommandType::EnterResearch"},
        {static_cast<unsigned>(bw::CommandType::ExitResearch), 14, "CommandType::ExitResearch"},
        {static_cast<unsigned>(bw::CommandType::RequestSync), 15, "CommandType::RequestSync"},
        // SettingType (CR-003) + FontId (02 §7)
        {static_cast<unsigned>(bw::SettingType::Bool), 0, "SettingType::Bool"},
        {static_cast<unsigned>(bw::SettingType::Int), 1, "SettingType::Int"},
        {static_cast<unsigned>(bw::SettingType::Float), 2, "SettingType::Float"},
        {static_cast<unsigned>(bw::FontId::Small), 0, "FontId::Small"},
        {static_cast<unsigned>(bw::FontId::Medium), 1, "FontId::Medium"},
        {static_cast<unsigned>(bw::FontId::Large), 2, "FontId::Large"},
    };

    for (const Row& row : rows) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s must keep value %u (append-only, 02 §6.6)", row.name,
                 row.expected);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(row.expected, row.actual, msg);
    }
}

void test_contract_setting_key_and_sizes(void) {
    // SettingKey (CR-003) is empty at M0 by design — no value can be pinned yet, but
    // its ABI (uint16_t) is part of the contract and its cast round-trip must work.
    static_assert(std::is_same_v<std::underlying_type_t<bw::SettingKey>, uint16_t>,
                  "SettingKey must stay uint16_t (CR-003)");
    static_assert(std::is_same_v<std::underlying_type_t<bw::SettingType>, uint8_t>,
                  "SettingType must stay uint8_t (CR-003)");
    const bw::SettingKey key = static_cast<bw::SettingKey>(0x1234);
    TEST_ASSERT_EQUAL_UINT32(0x1234, static_cast<unsigned>(key));
    TEST_PASS_MESSAGE("SettingKey underlying type and cast are intact");
}

void test_contract_event_offsets(void) {
    // CR-002's load-bearing order, as runtime-visible offsets (also static_asserted
    // in bw/contract_checks.h).
    TEST_ASSERT_EQUAL_UINT32(0, offsetof(bw::Event, t_start_ms));
    TEST_ASSERT_EQUAL_UINT32(8, offsetof(bw::Event, seq));
    TEST_ASSERT_EQUAL_UINT32(12, offsetof(bw::Event, boot_id));
    TEST_ASSERT_EQUAL_UINT32(16, offsetof(bw::Event, duration_ms));
    TEST_ASSERT_EQUAL_UINT32(20, offsetof(bw::Event, t_start_unix));
    TEST_ASSERT_EQUAL_UINT32(24, offsetof(bw::Event, confidence));
    TEST_ASSERT_EQUAL_UINT32(28, offsetof(bw::Event, reasons));
    TEST_ASSERT_EQUAL_UINT32(32, offsetof(bw::Event, payload));
    TEST_ASSERT_EQUAL_UINT32(40, offsetof(bw::Event, quality));
    TEST_ASSERT_EQUAL_UINT32(42, offsetof(bw::Event, type));
    TEST_ASSERT_EQUAL_UINT32(43, offsetof(bw::Event, label));
    TEST_ASSERT_EQUAL_UINT32(44, offsetof(bw::Event, context));
    TEST_ASSERT_EQUAL_UINT32(45, offsetof(bw::Event, alertness_at_start));
    TEST_ASSERT_EQUAL_UINT32(46, offsetof(bw::Event, detector_id));
    TEST_ASSERT_EQUAL_UINT32(47, offsetof(bw::Event, schema_version));
    TEST_ASSERT_EQUAL_UINT32(48, sizeof(bw::Event));
}
