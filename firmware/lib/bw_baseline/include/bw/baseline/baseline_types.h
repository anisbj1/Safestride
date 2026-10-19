#pragma once

// bw/baseline/baseline_types.h — F13 §7: pure value types of the personalized
// baseline. No platform calls, no time source: every input arrives through
// BaselineSample, which the F13 pump fills from AppContext mailboxes (G1
// ruling — bw_baseline depends on bw_core only, never on another feature's
// headers; the poisoning-guard and context inputs travel in this struct).

#include <bw/context.h>
#include <bw/quality.h>

#include <cstdint>

namespace bw::baseline {

// Why a sample was refused (F13 §9 asserts on these values). Library-local
// debugging vocabulary, not a cross-feature contract — not persisted/transmitted.
enum class SampleVerdict : uint8_t {
    Accepted,
    HoldActive,    // D5 poisoning guard: suspected alert/episode live or cooldown
    NotWorn,       // wear gate (§5 rule 1: no evidence gathered off-wrist)
    BadQuality,    // a source flag/quality not Good, hr invalid, or non-finite enmo
    BadActivity,   // outside the resting activity set (D2 widens it in Calibration)
    HrOutOfRange,  // outside the physiological plausibility band (NaN lands here)
    Outlier,       // post-ramp band rejection (D1)
};

// One learning request. The pump (wire_baseline.cpp) maps the four feature
// mailboxes plus the state/alert/episode context into this struct — the
// library never reads a mailbox itself (01 §1 rule 1 / G1 ruling 1).
// Deliberately time-free (D1/§9 test 17): v1 state has no timeline.
struct BaselineSample {
    float hr_bpm = 0.0f;
    float enmo_g = 0.0f;
    DataQuality ppg_quality = DataQuality::Insufficient;
    DataQuality motion_quality = DataQuality::Insufficient;
    // StateSnapshot::quality — the pump passes Good when the snapshot mailbox
    // is absent, so missing context alone never vetoes a sample (F13 §7).
    DataQuality snapshot_ppg = DataQuality::Good;
    DataQuality snapshot_imu = DataQuality::Good;
    bool hr_valid = false;
    bool motion_corrupted = false;
    WearState wear = WearState::Uncertain;
    ActivityContext activity = ActivityContext::Unknown;
    bool hold = false;         // D5: alert level >= Active or a live episode
    bool calibration = false;  // D2: OperatingMode::Calibration widens the activity gate
};

// Outcome of one tracker step. `valid_transition` = valid flipped false→true on
// this step — the extra persist trigger of D3/§8 (kPersistIntervalMs).
struct StepResult {
    bool accepted = false;
    SampleVerdict verdict = SampleVerdict::BadQuality;
    bool valid_transition = false;
};

// Persisted learner state = exactly D1's "3 floats + accepted count" (fits the
// 32 B blob, D3/§11). No TimeMs member on purpose (§9 test 17): a restore after
// any reboot gap is bit-identical to an uninterrupted run.
struct TrackerState {
    float hr_ewma = 0.0f;
    float hr_absdev_ewma = 0.0f;  // EWMA of |deviation| — the resting_hr_mad proxy (D1)
    float enmo_ewma = 0.0f;
    uint32_t accepted_samples = 0;
};

}  // namespace bw::baseline
