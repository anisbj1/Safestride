#pragma once

// bw/wake/wake_types.h — F06's local value types.
//
// These are NOT shared contracts: they stay inside bw_wake and no other
// feature may include this header (01 §1 rule 1). What crosses the feature
// boundary is `AlertnessProbabilities` from bw/model.h — plus the reason
// channel, which the current contract has no field for and which CR-010
// proposes to add (F06 spec §6 D3). Until it lands, WakeEstimate is F06's
// local adapter and its `probs` member is handed out unchanged.

#include <cstdint>
#include <type_traits>

#include <bw/context.h>
#include <bw/features.h>
#include <bw/model.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::wake {

// Local diagnostics only: not a shared, persisted or transmitted enum.
// The first failing gate explains invalid motion independently of confidence.
enum class WakeInputRejection : uint8_t {
    None,
    WearUnsafe,
    WearFuture,
    WearStale,
    SampleNotIncreasing,
    MotionFuture,
    MotionStale,
    ImuInsufficient,
    MotionInsufficient,
    EnmoInvalid,
    CadenceGap,
};

// One 1 s window: everything F06 consumes, assembled by the composition root
// from the channels F02/F03/F23 publish (01 §1 rule 2 — value types only).
//
// Absence is expressed by the contracts themselves, never by a magic value:
// MotionFeatures/PpgFeatures say "no data" with `quality = Insufficient`
// (bw/features.h), and an ActivityEstimate that was never produced reads back
// as Unknown/0 confidence, which the assembler refuses to use (F06 §5).
struct WakeSample {
    TimeMs t_ms;  // window end, monotonic (02 §1)
    MotionFeatures motion;
    PpgFeatures ppg;
    ActivityEstimate activity;
    SourceQuality quality;  // F23's per-source view; authoritative gate
};

// One completed epoch, folded down to fixed-size aggregates.
// RingBuffer holds kHistoryEpochs of these (F06 D4: accumulators, not raw
// windows — 10 x ~60 B instead of 10 x 30 x ~60 B).
struct EpochSummary {
    TimeMs t_ms;            // t_ms of the last window folded in
    uint16_t sample_count;  // windows folded into this epoch
    bool imu_usable;        // every gate passed for at least one window
    bool ppg_usable;
    DataQuality quality;  // worst quality the epoch may honestly claim
    float enmo_mean_g;
    float enmo_low_ratio;  // 0..1 share of usable windows below the anchor
    float mag_std_mean_g;
    float jerk_rms_mean_g_per_s;
    float stillness_max_s;              // longest continuous stillness, seconds
    float hr_mean_bpm;                  // mean over windows with a valid HR (0 if none)
    float hr_valid_ratio;               // 0..1
    float activity_confidence;          // mean over usable windows
    ActivityContext activity_dominant;  // most frequent usable window
    bool baseline_valid;
    float hr_z_baseline;  // (hr - resting)/mad_eff; 0 when !baseline_valid
    bool tod_valid;
    uint16_t tod_min;  // minutes since local midnight, 0..1439
};

// F06's output until CR-010 is decided: the contract probabilities, plus why
// they came out the way they did (F06 §3, consumed by F07/F22).
struct WakeEstimate {
    AlertnessProbabilities probs;
    ReasonMask reasons;  // bitset of ReasonCode (bw/state.h)
};

// Safety checks: everything here travels through queues/mailboxes by copy
// (02 §1) and must never carry padding garbage across a task boundary.
static_assert(std::is_trivially_copyable_v<WakeSample>, "02 §1: trivially copyable");
static_assert(std::is_trivially_copyable_v<EpochSummary>, "02 §1: trivially copyable");
static_assert(std::is_trivially_copyable_v<WakeEstimate>, "02 §1: trivially copyable");

// Number of ActivityContext values, derived from the enum's last member so a
// future append-only addition (02 §6.6) is caught by this header's users
// rather than silently undersizing the dominant-context histogram.
constexpr uint8_t kActivityContextCount =
    static_cast<uint8_t>(ActivityContext::InactiveProlonged) + 1;

// Posture/locomotion classes that carry no locomotion: these are what the
// schema's `activity_stationary` flag means. Walking and Running (and an
// Unknown estimate, which says nothing at all) map to 0 — Unknown must not
// read as "still", or the model would learn that no data means rest.
constexpr bool isStationaryContext(ActivityContext c) {
    switch (c) {
    case ActivityContext::Stationary:
    case ActivityContext::Sitting:
    case ActivityContext::Standing:
    case ActivityContext::LyingResting:
    case ActivityContext::InactiveProlonged:
        return true;
    default:
        return false;
    }
}

// Classes of AlertnessProbabilities, in the order the schema and IModel use:
// p[0] awake, p[1] drowsy, p[2] sleep (bw/model.h ModelOutput).
constexpr uint8_t kAlertnessClassCount = 3;

// Sets one bit of a ReasonMask (bw/state.h: a bitset of ReasonCode, codes
// append-only — 02 §6.6). Inline so every estimator reports reasons the same
// way and no one hand-rolls a shift with a magic offset.
inline void setReason(ReasonMask& mask, ReasonCode code) {
    mask |= (static_cast<ReasonMask>(1) << static_cast<uint8_t>(code));
}

}  // namespace bw::wake
