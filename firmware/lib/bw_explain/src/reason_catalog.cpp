#include <bw/explain/reason_catalog.h>

namespace bw {
namespace explain {
namespace {

// Short OLED strings (F22 §6 D1): one compact FACTOR label per ReasonCode,
// <= kMaxShortReasonChars chars, lowercase ASCII, no diagnosis/certainty
// wording (F22 §5) — all enforced by test_reason_catalog.cpp. The labels name
// the observable factor ('low motion'), never a medical claim; where a label
// implies a detection it reads 'possible' (e.g. 'possible fall').
const ReasonEntry kEntries[] = {
    {ReasonCode::SustainedLowMotion, "low motion"},
    {ReasonCode::ProlongedInactivity, "still too long"},
    {ReasonCode::HrBelowBaseline, "low hr"},
    {ReasonCode::HrTrendChange, "hr change"},
    {ReasonCode::AbruptMotionDrop, "motion dropped"},
    {ReasonCode::TimeOfDayTypicalDrowsy, "typical time"},
    {ReasonCode::MatchesPastConfirmedEvent, "past pattern"},
    {ReasonCode::UserMarked, "user marked"},
    {ReasonCode::PlannedNap, "planned nap"},
    {ReasonCode::LowSignalQuality, "weak signal"},
    {ReasonCode::SensorUnavailable, "sensor missing"},
    {ReasonCode::DeviceNotWorn, "not worn"},
    {ReasonCode::SustainedLowAlertnessProbability, "low alertness"},
    {ReasonCode::SustainedWakefulnessProbability, "wake sustained"},
    {ReasonCode::InsufficientEvidence, "no clear sign"},
    {ReasonCode::PossibleFall, "possible fall"},
    {ReasonCode::PostureChange, "posture change"},
    {ReasonCode::ShortMotionBurst, "short burst"},
};

// The table must cover exactly the enumerator count derived from bw_core's
// enum (kReasonCatalogCount): more entries fail here, fewer are caught at
// runtime because the missing codes then resolve to nullptr — and both cases
// are also caught compile/runtime-side by test_reason_catalog.cpp (F22 §9 D2).
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == kReasonCatalogCount,
              "bw_explain catalog size must equal the ReasonCode count");

}  // namespace

const char* reasonText(ReasonCode code) {
    for (const ReasonEntry& entry : kEntries) {
        if (entry.code == code) {
            return entry.short_text;
        }
    }
    return nullptr;
}

}  // namespace explain
}  // namespace bw
