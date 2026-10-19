#include <bw/signal/heart_rate_estimator.h>

namespace bw::signal {

HeartRateResult HeartRateEstimator::estimate(const RingBuffer<TimeMs, kMaxBeatsInHrWindow>& beats,
                                             TimeMs window_end_ms) {
    HeartRateResult out{};
    out.valid = false;

    const TimeMs range_start = window_end_ms > kHrWindowMs ? window_end_ms - kHrWindowMs : 0;

    // Collect in-window beats (ring is oldest-first) and their IBIs.
    TimeMs stamps[kMaxBeatsInHrWindow];
    uint32_t stamp_count = 0;
    for (size_t i = 0; i < beats.size(); ++i) {
        const TimeMs t = *beats.at(i);
        if (t > range_start && t <= window_end_ms) {
            stamps[stamp_count++] = t;
        }
    }
    out.beat_count = stamp_count;

    if (stamp_count < 2) {
        return out;  // no IBI yet — consistency and median stay 0
    }

    TimeMs ibis[kMaxBeatsInHrWindow];
    uint32_t ibi_count = 0;
    for (uint32_t i = 1; i < stamp_count; ++i) {
        ibis[ibi_count++] = stamps[i] - stamps[i - 1];
    }

    // Median via insertion sort (ibi_count <= 39, O(n^2) is irrelevant here
    // and the fixed array keeps the stack bounded — no heap, no <algorithm>).
    for (uint32_t i = 1; i < ibi_count; ++i) {
        const TimeMs key = ibis[i];
        uint32_t j = i;
        while (j > 0 && ibis[j - 1] > key) {
            ibis[j] = ibis[j - 1];
            --j;
        }
        ibis[j] = key;
    }
    const float median_ibi_ms = (ibi_count % 2u) == 1u
                                    ? static_cast<float>(ibis[ibi_count / 2u])
                                    : 0.5f * (static_cast<float>(ibis[ibi_count / 2u - 1u]) +
                                              static_cast<float>(ibis[ibi_count / 2u]));
    out.median_ibi_ms = median_ibi_ms;

    // Consistency: fraction of IBIs inside the tolerance band (F02 D6).
    const float lo = median_ibi_ms * (1.0f - kIbiMedianToleranceFrac);
    const float hi = median_ibi_ms * (1.0f + kIbiMedianToleranceFrac);
    // FIX loop 4 (bench round 7): the missed-beat band — ~2x the median is
    // ONE missed beat (its amplitude dipped below the detection floor after
    // the ratchet fix), not arrhythmia; a 3x+ gap stays outside both bands
    // and remains a rhythm break. The raw median is deliberately unchanged:
    // with ≥ ~50 % misses it skews into no band and the train honestly
    // fails (self-limiting — see kIbiMissedBeatMultiple).
    const float lo2 = kIbiMissedBeatMultiple * median_ibi_ms * (1.0f - kIbiMedianToleranceFrac);
    const float hi2 = kIbiMissedBeatMultiple * median_ibi_ms * (1.0f + kIbiMedianToleranceFrac);
    // FIX loop 5 (bench round 7): TWO horizons. The strict `consistency`
    // (SQI, hr_confidence) counts every IBI of the whole lookback — the
    // window's reliability must not forget an old miss. The VALIDITY
    // fraction (consistency_miss_tolerant) counts only the newest
    // kHrRecentIbiCount IBIs so a short weak/motion stretch ages out of the
    // gate in seconds instead of holding hr_valid=false for the rest of the
    // kHrWindowMs lookback (round 7: a ~4 s weak stretch produced 14 s
    // `--` flickers under steady contact). Counting walks the CHRONOLOGICAL
    // stamps (the in-place sort above destroyed ibis' order) — same
    // multiset, so the full-window number is unchanged.
    const uint32_t recent_start =
        ibi_count > kHrRecentIbiCount ? ibi_count - kHrRecentIbiCount : 0u;
    uint32_t near = 0;
    uint32_t recent_near = 0;
    uint32_t recent_miss = 0;
    for (uint32_t i = 0; i < ibi_count; ++i) {
        const float ibi = static_cast<float>(stamps[i + 1] - stamps[i]);
        const bool in_band = ibi >= lo && ibi <= hi;
        if (in_band) {
            ++near;
        }
        if (i >= recent_start) {
            if (in_band) {
                ++recent_near;
            } else if (ibi >= lo2 && ibi <= hi2) {
                ++recent_miss;
            }
        }
    }
    out.consistency = static_cast<float>(near) / static_cast<float>(ibi_count);
    out.consistency_miss_tolerant = static_cast<float>(recent_near + recent_miss) /
                                    static_cast<float>(ibi_count - recent_start);

    // Gates (F02 D6). The median must be physiologically possible at all:
    // faster than kPpgRefractoryMs (200 bpm) or slower than the 30 bpm band
    // edge (+ margin) means the timing is wrong, not the heart rate. The
    // VALIDITY gate uses the miss-tolerant fraction over the newest
    // kHrRecentIbiCount IBIs (FIX loops 4+5); the quality gate (SQI) keeps
    // consuming the strict full-window one.
    const bool beats_ok = stamp_count >= kMinBeatsForHr;
    const bool median_ok = median_ibi_ms >= static_cast<float>(kPpgRefractoryMs) &&
                           median_ibi_ms <= static_cast<float>(kIbiPlausibleMaxMs);
    const bool timing_ok = out.consistency_miss_tolerant >= kMinIbiConsistencyFraction;

    if (beats_ok && median_ok && timing_ok) {
        out.valid = true;
        out.hr_bpm = static_cast<float>(kMsPerMinute) / median_ibi_ms;
    }
    return out;
}

}  // namespace bw::signal
