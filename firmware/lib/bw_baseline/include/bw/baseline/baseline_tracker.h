#pragma once

// bw/baseline/baseline_tracker.h — F13 D1/D4: the pure EWMA learner and
// IBaselineProvider implementation. Single writer (the F13 pump calls step()),
// NOT thread-safe by design — cross-task readers go through the wiring
// adapter's LatestValue (f13::BaselineProviderAdapter, D4), never by calling
// profile() from another task.

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_types.h>
#include <bw/baseline/qualified_sample_filter.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw::baseline {

class BaselineTracker : public bw::IBaselineProvider {
public:
    explicit BaselineTracker(const BaselineConfig& cfg = BaselineConfig{})
        : cfg_(cfg), filter_(cfg) {}

    // Accepts or refuses one sample (pure, no platform calls, no time).
    // Rejected samples leave state_ bit-identical (§9 tests 5/8).
    StepResult step(const BaselineSample& sample);

    // IBaselineProvider: false until the first accepted sample — "no baseline
    // yet" is an honest absent answer (02 §5), never fabricated values (§9 1).
    bool profile(bw::BaselineProfile& out) const override;

    const TrackerState& state() const { return state_; }

    // Warm restart (D3/§10): validates a deserialized state before adopting it.
    // Refuses non-finite or out-of-band values with Err::Corrupt and leaves the
    // tracker untouched, so the caller discards the blob and restarts learning.
    bw::Result<void> restore(const TrackerState& in);

private:
    // valid ⇔ kMinQualifiedMs of accepted evidence (u32 samples -> ms via
    // kMsPerSecond, done in u64 so the multiply never wraps).
    bool isValid() const;
    // Linear evidence ramp to kFullConfidenceSamples, clamped to percent (§8).
    uint8_t confidencePct() const;

    BaselineConfig cfg_;
    QualifiedSampleFilter filter_;
    TrackerState state_{};
    // D5 cooldown, runtime-only and deliberately NOT part of TrackerState: the
    // 32 B blob is full and a reboot itself separates the learner from the
    // event context that armed it.
    uint32_t hold_cooldown_ = 0;
};

}  // namespace bw::baseline
