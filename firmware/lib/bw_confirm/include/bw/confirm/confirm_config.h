#pragma once

// bw/confirm/confirm_config.h — F14 §8 (named constants, every value tagged and
// mirrored in the spec table) plus the wiring-owned configuration struct (D6).
//
// F14 prompts the user to confirm/reject detected events (00 §1 prototype, never
// a diagnosis). Every value below is a starting point until F28/F29 recordings
// exist; each carries its tag and reasoning — never a bare number (00 §7.5).

#include <array>
#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::confirm {

// Bit for a single ReasonCode. Feature-local on purpose: a feature may only
// include bw_core headers (01 §1 rule 1), so each feature that needs a reason
// bit carries this one-line helper (bw/cataplexy/cataplexy_config.h precedent).
constexpr ReasonMask reasonBit(ReasonCode code) {
    return ReasonMask{1} << static_cast<uint8_t>(code);
}

// ---- prompt timing (spec §8 / D1) ------------------------------------------

// [EXPERIMENTAL] (spec §8) How long an Offered prompt waits for an answer
// before resolving to Unlabeled. 10 s (was 30 s; prototype demo value,
// developer 2026-10-18): the wakefulness-check window in the demo matches
// F10's Warning->Active timer. Why 30 s originally: long enough for a drowsy
// user to notice and respond, short enough that a stale prompt never
// lingers - re-tune target at Validate.
constexpr TimeMs kPromptTimeoutMs = 10000;

// [EXPERIMENTAL] (spec §8) Delay between the alert being acknowledged and the
// first prompt. Why 5 s: lets the user become alert first (D1) — a prompt shown
// mid-alert would collide with the alert UI and "must not block alerts".
constexpr TimeMs kPromptDeferAfterAckMs = 5000;

// ---- queue bounds (spec §8 / D1) -------------------------------------------

// [EXPERIMENTAL] (spec §8) Events waiting to be prompted. Bounds RAM and user
// burden: 4 x 16 B = 64 B of queue, and at most 4 unanswered prompts before the
// oldest resolves Unlabeled (spec §9 overflow rule).
constexpr size_t kMaxPending = 4;

// [DERIVED] Slots in ConfirmConfig::eligible_types. Why 4: the default set
// (kPromptEligibleDefault) has 2 entries; 2 spare slots let wiring widen the
// policy later without growing the struct or re-compiling bw_core.
constexpr size_t kMaxEligibleTypes = 4;

// [EXPERIMENTAL] (spec §8) Default prompt-eligibility set (D6): F14 never
// decides what an event is — it prompts only for the types the configuration
// names. Why these two: the detections whose "possible/suspected" label the
// user is asked to confirm; system events (Boot, TimeSync, ...) never prompt.
constexpr std::array<EventType, 2> kPromptEligibleDefault = {EventType::DrowsinessEvent,
                                                             EventType::SuspectedCataplexy};

// ---- store-failure retry (spec §10 / D7) -----------------------------------

// [EXPERIMENTAL] (spec §10) Total setLabel attempts per label (first try +
// retries) before it is given up and counted. Why 3: bounded RAM and step()
// time while covering a transient Err::Full; the label stays visible in RAM
// stats and F21 can still late-label (D1).
constexpr uint8_t kRetryCount = 3;

// [EXPERIMENTAL] (spec §10, plan §11) Labels held for a store retry. Why 4:
// one record (8 B) per queued label — at most kMaxPending failed labels can be
// in flight, so nothing extra is ever dropped.
constexpr size_t kMaxRetryPending = 4;

// ---- identity (plan §8, integrator G0 registry) --------------------------

// [DERIVED] detector_id reserved for F14 by the integrator registry (G0).
// Why: marks events F14 itself emits (label-change notifications, D5) so its
// own onEvent can loop-guard on them, and avoids collision with the detector
// ids other features own.
constexpr uint8_t kDetectorId = 14;

// ---- configuration (D6) ----------------------------------------------------

// Policy struct supplied/read at wiring time. Eligibility is configuration,
// not logic: what deserves a prompt is a wiring decision (D6); the manager
// only filters on it and never interprets event semantics (spec §5).
struct ConfirmConfig {
    TimeMs prompt_timeout_ms = kPromptTimeoutMs;
    TimeMs defer_after_ack_ms = kPromptDeferAfterAckMs;
    uint8_t retry_count = kRetryCount;

    // First eligible_count entries of eligible_types are the prompt-eligible
    // EventTypes; remaining slots are never read — fill before raising the
    // count (kPromptEligibleDefault fills the first two).
    uint8_t eligible_count = static_cast<uint8_t>(kPromptEligibleDefault.size());
    EventType eligible_types[kMaxEligibleTypes] = {kPromptEligibleDefault[0],
                                                   kPromptEligibleDefault[1]};

    bool isEligible(EventType type) const {
        for (uint8_t i = 0; i < eligible_count; ++i) {
            if (eligible_types[i] == type) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace bw::confirm
