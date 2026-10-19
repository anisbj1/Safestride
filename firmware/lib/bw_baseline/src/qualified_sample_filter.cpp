#include <bw/baseline/qualified_sample_filter.h>

#include <cmath>

namespace bw::baseline {

namespace {

// Table-driven membership over the §8 activity sets (D2: Calibration swaps in
// the wider table, nothing else changes).
bool activityQualifies(ActivityContext activity, bool calibration) {
    if (calibration) {
        for (const ActivityContext c : kQualifiedActivityCalibration) {
            if (c == activity) {
                return true;
            }
        }
        return false;
    }
    for (const ActivityContext c : kQualifiedActivity) {
        if (c == activity) {
            return true;
        }
    }
    return false;
}

}  // namespace

SampleVerdict QualifiedSampleFilter::check(const BaselineSample& s) const {
    // D5 / §5 rule 2: poisoning first (see header).
    if (s.hold) {
        return SampleVerdict::HoldActive;
    }
    if (s.wear != WearState::Worn) {
        return SampleVerdict::NotWorn;
    }
    // §5 rule 1: all four sources must be Good; hr_valid and !motion_corrupted
    // are part of the quality_good formula pinned in §7 (both features own
    // those flags — the pump copies them into the sample, the gate just ANDs).
    if (s.ppg_quality != DataQuality::Good || s.motion_quality != DataQuality::Good ||
        s.snapshot_ppg != DataQuality::Good || s.snapshot_imu != DataQuality::Good || !s.hr_valid ||
        s.motion_corrupted) {
        return SampleVerdict::BadQuality;
    }
    // Negated inclusive comparison so a NaN hr lands in HrOutOfRange instead of
    // sliding through both < and > checks (never learn from absurd data, 00 §7).
    if (!(s.hr_bpm >= cfg_.min_resting_hr_bpm && s.hr_bpm <= cfg_.max_resting_hr_bpm)) {
        return SampleVerdict::HrOutOfRange;
    }
    // Same NaN trick for ENMO: a non-finite value is bad data (00 §7.5) and a
    // negative ENMO is physically impossible — neither may reach the EWMA.
    if (!std::isfinite(s.enmo_g) || !(s.enmo_g >= 0.0f)) {
        return SampleVerdict::BadQuality;
    }
    if (!activityQualifies(s.activity, s.calibration)) {
        return SampleVerdict::BadActivity;
    }
    return SampleVerdict::Accepted;
}

}  // namespace bw::baseline
