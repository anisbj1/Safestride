#pragma once

// Shared fixtures for the F13 host suite — private test helpers (host-testing
// test helpers live under the feature's own test/ folder, they are not a
// shared bw_testkit fake).

#include <bw/baseline/baseline_types.h>

#include <bw/context.h>
#include <bw/quality.h>

namespace f13_test {

// A fully qualified resting window (worn, both sources Good, snapshot sources
// Good, plausible HR, resting activity, no hold, no calibration): a test varies
// exactly one field to hit one gate.
inline bw::baseline::BaselineSample goodSample(float hr_bpm = 60.0f, float enmo_g = 0.05f) {
    bw::baseline::BaselineSample s{};
    s.hr_bpm = hr_bpm;
    s.enmo_g = enmo_g;
    s.ppg_quality = bw::DataQuality::Good;
    s.motion_quality = bw::DataQuality::Good;
    s.snapshot_ppg = bw::DataQuality::Good;
    s.snapshot_imu = bw::DataQuality::Good;
    s.hr_valid = true;
    s.motion_corrupted = false;
    s.wear = bw::WearState::Worn;
    s.activity = bw::ActivityContext::Sitting;
    s.hold = false;
    s.calibration = false;
    return s;
}

// Value equality of learner state; the struct is 3 f32 + u32 with no padding
// (size pinned by test 16), so equal values mean an identical learner.
inline bool sameState(const bw::baseline::TrackerState& a, const bw::baseline::TrackerState& b) {
    return a.hr_ewma == b.hr_ewma && a.hr_absdev_ewma == b.hr_absdev_ewma &&
           a.enmo_ewma == b.enmo_ewma && a.accepted_samples == b.accepted_samples;
}

}  // namespace f13_test
