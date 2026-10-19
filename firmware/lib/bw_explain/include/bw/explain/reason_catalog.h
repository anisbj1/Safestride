// bw_explain — versioned short-string catalog for bw::ReasonCode (F22 §6 D1).
//
// The device owns SHORT strings only: one compact factor label per ReasonCode,
// rendered on the OLED / in event text through explain_short.h. The full
// explanatory text and contribution ranking live in the companion (deferred to
// W5) and are versioned against kReasonCatalogVersion so a phone can detect a
// catalog mismatch instead of rendering text for the wrong code.
#pragma once

#include <bw/state.h>

namespace bw {
namespace explain {

// [DERIVED] Bump on ANY change to the code<->text mapping: an added ReasonCode
// entry or reworded text. Why: core §1 requires a version on values that are
// transmitted/persisted; the companion (W5) compares its catalog version with
// this one and refuses to render on a mismatch.
constexpr uint16_t kReasonCatalogVersion = 1;

// [DERIVED] Entries = enumerators of bw::ReasonCode, derived from the enum's
// LAST NAMED member (append-only, docs/interfaces/data_decision.md §4.5) —
// deliberately not a literal count, so the number stays tied to the bw_core
// enum. Appending a code does not change this value; the completeness test
// (test_reason_catalog.cpp, F22 §9 D2) parses bw/state.h and fails until the
// catalog gains the matching entry.
constexpr uint8_t kReasonCatalogCount = static_cast<uint8_t>(ReasonCode::ShortMotionBurst) + 1;

struct ReasonEntry {
    ReasonCode code;
    // Never nullptr, ASCII printable, <= bw::explain::kMaxShortReasonChars
    // (see explain_short.h); enforced by test_reason_catalog.cpp.
    const char* short_text;
};

// Text for one code, or nullptr when the catalog has no entry (e.g. a code
// appended to bw_core before F22 caught up). Never a silent placeholder:
// callers that must render use explainShort(), which prints 'reason #N'
// for the missing bit (F22 §10).
const char* reasonText(ReasonCode code);

}  // namespace explain
}  // namespace bw
