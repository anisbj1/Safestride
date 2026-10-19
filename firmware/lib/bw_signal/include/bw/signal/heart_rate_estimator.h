#pragma once

// bw/signal/heart_rate_estimator.h — F02 D2: heart rate from the MEDIAN
// inter-beat interval over the kHrWindowMs lookback. The median is robust to
// missed/extra beats; consistency (fraction of IBIs within
// kIbiMedianToleranceFrac of the median) is the timing-quality number the
// SQI consumes (F02 D6). Pure function of the beat stamps — no state.

#include <bw/containers/ring_buffer.h>
#include <bw/signal/signal_config.h>
#include <bw/types.h>

namespace bw::signal {

struct HeartRateResult {
    bool valid;         // passes ALL timing gates (quality gate is separate)
    float hr_bpm;       // 0 when invalid (flag carries the meaning, 02 §1)
    float consistency;  // fraction of IBIs near the median (1x band only)
    // FIX loop 4: same as `consistency` but an IBI of ~kIbiMissedBeatMultiple
    // x the median (ONE missed beat) also counts. This is the fraction the
    // validity gate uses; `consistency` stays strict because SQI and
    // hr_confidence must keep penalising every missed beat honestly.
    // FIX loop 5: this validity fraction is measured over the NEWEST
    // kHrRecentIbiCount IBIs only (a poisoned stretch must age out of the
    // gate in seconds, not linger for the whole lookback); `consistency`
    // and the median stay full-window.
    float consistency_miss_tolerant;
    float median_ibi_ms;  // median of the in-window IBIs, 0 when none
    uint32_t beat_count;  // beats stamped inside the lookback window
};

class HeartRateEstimator {
public:
    // Estimates HR from beat stamps in (window_end_ms - kHrWindowMs,
    // window_end_ms]. Gates: beat_count >= kMinBeatsForHr, median inside
    // [kPpgRefractoryMs, kIbiPlausibleMaxMs], miss-tolerant consistency over
    // the newest kHrRecentIbiCount IBIs >= kMinIbiConsistencyFraction
    // (F02 D6, FIX loop 5). Signal-quality gating and the hr_valid lifetime
    // policy (hold / beat-gap) happen in SignalProcessor, not here.
    static HeartRateResult estimate(const RingBuffer<TimeMs, kMaxBeatsInHrWindow>& beats,
                                    TimeMs window_end_ms);
};

}  // namespace bw::signal
