#pragma once

// bw/state.h — docs/02_INTERFACES.md §4.5 (state axes; F07 composes them).
// Independent axes per DEC-004/00 §6: alertness, wear, activity, quality, mode and
// power are separate values — never one combinatorial enum.
// Enums are append-only (02 §6.6); numeric values are pinned by
// test_contract_layout.cpp.

#include <cstdint>

#include <bw/context.h>
#include <bw/quality.h>
#include <bw/types.h>

namespace bw {

enum class AlertnessState : uint8_t {
    Unknown,
    NotWorn,
    Awake,
    Drowsy,
    HighDrowsinessRisk,
    PossibleSleep,
};

enum class OperatingMode : uint8_t { Day, Night, Nap, Calibration, Research };

enum class PowerProfile : uint8_t { Normal, LowPower };  // orthogonal to mode (00 §6)

// Bitset of ReasonCode; F22 owns the human-readable text, the codes live in bw_core.
using ReasonMask = uint32_t;

enum class ReasonCode : uint8_t {
    SustainedLowMotion,
    ProlongedInactivity,
    HrBelowBaseline,
    HrTrendChange,
    AbruptMotionDrop,
    TimeOfDayTypicalDrowsy,
    MatchesPastConfirmedEvent,
    UserMarked,
    PlannedNap,
    LowSignalQuality,
    SensorUnavailable,
    DeviceNotWorn,
    // CR-008 (accepted 2026-10-11, integrator G0): state-engine justification
    // codes (F07). Bits 12–14.
    SustainedLowAlertnessProbability,  // smoothed alertness probabilities held above the
                                       // state's enter threshold for its minimum duration (F07
                                       // D8/E1)
    SustainedWakefulnessProbability,   // calm evidence held for the recovery dwell (F07 D8/E3)
    InsufficientEvidence,              // inputs valid but no state was sustained (F07 D8/E3)
    // CR-009 (accepted 2026-10-11, integrator G0): F09 confounder diagnostics,
    // appended after CR-008 (landing order decided at G0). Bits 15–17. These are
    // diagnostics on a suspected event, never a claim that a fall/posture change occurred.
    PossibleFall,      // F09 §6 D2: descent looked uncontrolled (jerk/impact spike)
    PostureChange,     // F09 §6 D2: wrist re-oriented across the drop (sitting/laying down)
    ShortMotionBurst,  // F09 §6 D2: activity lasted less than the look-back (gesture)
    // … append only, never renumber (02 §6.6)
};

// CR-007 (accepted 2026-10-11, integrator G0): F08 produces, F11/F12 consume
// (mailbox: app-owned LatestValue<EpisodeStatus>). Append-only enums: never
// renumber or reorder (02 §6.6). Not persisted or transmitted, so no
// kSchemaVersion (02 §1). Lives here because bw/state.h is 02 §4.5's header
// (F00's one-header-per-section map, F00 §6 D4).
enum class EpisodePhase : uint8_t { Idle, Candidate, Active };
enum class EpisodeClass : uint8_t { Uncertain, Rest, Nap, MainSleep };  // Uncertain == 0 on purpose

struct EpisodeStatus {
    TimeMs t_ms;        // when this status was produced
    TimeMs t_start_ms;  // monotonic start of the current/last episode; 0 when Idle
    EpisodePhase phase;
    EpisodeClass
        episode_class;  // provisional while phase != Idle; final value is in the close event
    float confidence;   // running confidence 0..1 (F08 §6 D5)
};

// CR-034: current F06 motion-window evidence, not contact or held Awake.
// Forward the original timestamp unchanged; consumers must check freshness.
struct WakeMotionEvidence {
    TimeMs t_ms;
    bool valid;
    bool qualifying;
};

struct StateSnapshot {
    TimeMs t_ms;
    AlertnessState alertness;
    WearState wear;
    ActivityContext activity;
    SourceQuality quality;
    OperatingMode mode;
    PowerProfile power;
    uint32_t in_state_ms;            // time spent in `alertness` so far
    float confidence;                // 0..1
    ReasonMask reasons;              // why the current alertness state was chosen
    WakeMotionEvidence wake_motion;  // CR-034; independent of probabilistic state
};

}  // namespace bw
