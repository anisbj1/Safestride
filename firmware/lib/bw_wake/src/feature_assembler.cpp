// bw/wake/feature_assembler.cpp — F06's assembly half: gating, epoching,
// history, schema vector. Pure logic: no FreeRTOS/Arduino/ESP-IDF calls, so
// the whole file compiles for `pio test -e native` (01 §1 rule 5).

#include <bw/wake/feature_assembler.h>

#include <cmath>

#include <bw/wake/wake_math.h>

namespace bw::wake {
namespace {

// bw/quality.h declares the enum in severity order (Good < Degraded <
// Insufficient), which is what lets us rank qualities without a switch. The
// assertion below pins that assumption to the contract (02 §4.1).
static_assert(static_cast<uint8_t>(DataQuality::Good) < static_cast<uint8_t>(DataQuality::Degraded),
              "quality ranking assumes Good < Degraded");
static_assert(static_cast<uint8_t>(DataQuality::Degraded) <
                  static_cast<uint8_t>(DataQuality::Insufficient),
              "quality ranking assumes Degraded < Insufficient");

uint8_t qualityRank(DataQuality q) {
    return static_cast<uint8_t>(q);
}

// The worse of two qualities: one bad source never gets averaged away
// (00 §1 — "no reliable data" beats a guess).
DataQuality worstQuality(DataQuality a, DataQuality b) {
    return qualityRank(a) >= qualityRank(b) ? a : b;
}

// A source may be used down to `min_allowed` inclusive (kMinInputQuality).
bool isUsable(DataQuality q, DataQuality min_allowed) {
    return qualityRank(q) <= qualityRank(min_allowed);
}

}  // namespace

// --- history-derived evidence ----------------------------------------------

const EpochSummary* previousEpoch(const EpochHistory& history) {
    if (history.size() < 2) {
        return nullptr;  // only the current epoch (or nothing) is stored
    }
    const EpochSummary* prev = history.at(history.size() - 2);
    // A trend reference must be comparable: an epoch that missed the IMU
    // quorum (imu_usable == false) holds at most a handful of windows, so
    // comparing across it would bridge a data gap with fabricated evidence
    // (F06 §5: no data is not a trend). Break instead of skipping older —
    // an older epoch is further from the gap, not closer to comparable.
    if (prev == nullptr || !prev->imu_usable) {
        return nullptr;
    }
    return prev;
}

uint16_t lowMotionEpochStreak(const EpochSummary& epoch, const EpochHistory& history,
                              const WakeConfig& cfg) {
    // imu_usable already carries the quorum (>= min_samples_per_epoch usable
    // windows, FeatureAssembler::commit), so it is the epoch's usable-sample
    // threshold as well as its gate: a sparse epoch is a streak-breaker, not
    // a silent gap in the run.
    if (!epoch.imu_usable || !(epoch.enmo_low_ratio >= cfg.low_motion_epoch_ratio)) {
        return 0;  // the ratio test also catches NaN: no evidence at all
    }
    uint16_t streak = 1;
    if (history.empty()) {
        return streak;
    }
    // history()'s newest slot is `epoch` itself (see feature_assembler.h), so
    // the walk starts one slot older and runs newest -> oldest.
    size_t idx = history.size() - 1;
    while (idx > 0) {
        --idx;
        const EpochSummary* e = history.at(idx);
        if (e == nullptr || !e->imu_usable || !(e->enmo_low_ratio >= cfg.low_motion_epoch_ratio)) {
            break;  // a busy OR an unusable epoch ends the run
        }
        ++streak;
        if (streak >= cfg.sleep_low_motion_epochs) {
            break;  // full evidence reached; no need to walk further
        }
    }
    return streak;
}

float enmoDropRatio(const EpochSummary& epoch, const EpochHistory& history) {
    // previousEpoch() already rejects a non-comparable (unusable) reference,
    // so a data gap yields no trend rather than a fabricated drop.
    const EpochSummary* prev = previousEpoch(history);
    if (prev == nullptr || !(prev->enmo_mean_g > 0.0f)) {
        return 0.0f;  // no reference epoch (or a motionless one) = no trend
    }
    return clamp01(1.0f - (epoch.enmo_mean_g / prev->enmo_mean_g));
}

bool hrEvidenceUsable(const EpochSummary& epoch, const WakeConfig& cfg) {
    return epoch.baseline_valid && (epoch.hr_valid_ratio >= cfg.min_hr_valid_ratio);
}

// --- schema vector ----------------------------------------------------------

Result<void> buildFeatureVector(const WakeConfig& cfg, const EpochSummary& e, const EpochHistory& h,
                                TimeMs t_ms, FeatureVector& out) {
    if (e.sample_count == 0) {
        return {Err::NotReady};  // nothing was ever accumulated: no vector
    }

    out.schema_id = kAlertnessSchemaId;
    out.schema_version = kAlertnessSchemaVersion;
    out.count = kAlertnessFeatureCount;
    out.t_ms = t_ms;

    // Raw (un-normalized) values, in normative feature order. Missing
    // numerics are written at their normalization identity — the schema mean
    // for "standardize" methods, 0 for "none" — so they land exactly on
    // normalized 0 (alertness_schema.h missingness rule), and a companion
    // flag/ratio feature records that they are missing.
    float raw[kAlertnessFeatureCount];
    const bool hr_mean_ok = e.hr_valid_ratio > 0.0f;
    const bool baseline_ok = hrEvidenceUsable(e, cfg);

    raw[0] = e.enmo_mean_g;
    raw[1] = e.enmo_low_ratio;
    raw[2] = e.mag_std_mean_g;
    raw[3] = e.jerk_rms_mean_g_per_s;
    raw[4] = e.stillness_max_s;
    raw[5] = hr_mean_ok ? e.hr_mean_bpm : kAlertnessFeatureMean[5];
    raw[6] = e.hr_valid_ratio;
    raw[7] = baseline_ok ? e.hr_z_baseline : 0.0f;
    raw[8] = baseline_ok ? 1.0f : 0.0f;
    raw[9] = isStationaryContext(e.activity_dominant) ? 1.0f : 0.0f;
    raw[10] = e.activity_confidence;
    raw[13] = e.tod_valid ? 1.0f : 0.0f;
    raw[11] = 0.0f;
    raw[12] = 0.0f;
    if (e.tod_valid) {
        const float angle =
            kTwoPi * (static_cast<float>(e.tod_min) / static_cast<float>(kMinutesPerDay));
        raw[11] = std::sin(angle);
        raw[12] = std::cos(angle);
    }
    raw[14] = static_cast<float>(lowMotionEpochStreak(e, h, cfg));
    raw[15] = enmoDropRatio(e, h);

    // FEATURE_SCHEMA §4: one formula for every feature, no branch on method
    // ("none" is stored as mean 0 / std 1 by the schema).
    for (uint8_t i = 0; i < kAlertnessFeatureCount; ++i) {
        out.v[i] = (raw[i] - kAlertnessFeatureMean[i]) / kAlertnessFeatureStd[i];
    }
    return {Err::Ok};
}

// --- assembler --------------------------------------------------------------

FeatureAssembler::FeatureAssembler(const WakeConfig& cfg, const IClock* clock,
                                   const IBaselineProvider* baseline)
    : cfg_(cfg), clock_(clock), baseline_(baseline), history_(), acc_(), epoch_index_(0),
      acc_active_(false) {
    clearAccumulators();
}

void FeatureAssembler::clearAccumulators() {
    acc_ = Accumulators{};
    acc_.worst_imu = DataQuality::Good;
    acc_.worst_ppg = DataQuality::Good;
}

void FeatureAssembler::reset() {
    history_.clear();
    clearAccumulators();
    epoch_index_ = 0;
    acc_active_ = false;
}

void FeatureAssembler::accumulate(const WakeSample& s) {
    acc_.t_ms = s.t_ms;
    ++acc_.windows;

    const DataQuality imu_q =
        worstQuality(worstQuality(s.quality.imu, s.motion.quality), acc_.worst_imu);
    const DataQuality ppg_q =
        worstQuality(worstQuality(s.quality.ppg, s.ppg.quality), acc_.worst_ppg);
    acc_.worst_imu = imu_q;
    acc_.worst_ppg = ppg_q;

    if (isUsable(imu_q, cfg_.min_input_quality)) {
        ++acc_.imu_windows;
        acc_.enmo_sum_g += s.motion.enmo_g;
        if (s.motion.enmo_g < cfg_.low_motion_enmo_g) {
            ++acc_.low_motion_windows;
        }
        acc_.mag_std_sum_g += s.motion.mag_std_g;
        acc_.jerk_sum_g_per_s += s.motion.jerk_rms_g_per_s;
        const float still_s =
            static_cast<float>(s.motion.stillness_ms) / static_cast<float>(kMsPerSecond);
        if (still_s > acc_.stillness_max_s) {
            acc_.stillness_max_s = still_s;
        }
    }

    if (isUsable(ppg_q, cfg_.min_input_quality)) {
        ++acc_.ppg_windows;
        // hr_valid is F02's contract; the >= 0 test additionally rejects NaN
        // (IEEE-754: every comparison with NaN is false) so one broken window
        // cannot poison the epoch mean.
        if (s.ppg.hr_valid && s.ppg.hr_bpm >= 0.0f) {
            ++acc_.hr_windows;
            acc_.hr_sum_bpm += s.ppg.hr_bpm;
        }
    }

    // Activity is derived from the IMU, so it inherits the IMU's gate; an
    // Unknown estimate or one below F03's own reporting floor carries no
    // information (F06 §5: no data is not "sedentary").
    if (isUsable(imu_q, cfg_.min_input_quality) && s.activity.context != ActivityContext::Unknown &&
        s.activity.confidence >= cfg_.min_activity_confidence) {
        ++acc_.activity_windows;
        acc_.activity_conf_sum += s.activity.confidence;
        const uint8_t idx = static_cast<uint8_t>(s.activity.context);
        if (idx < kActivityContextCount) {  // defensive: reject a corrupt byte
            ++acc_.activity_hist[idx];
        }
    }
}

void FeatureAssembler::commit(EpochSummary& out) {
    out.t_ms = acc_.t_ms;
    out.sample_count = acc_.windows;

    // Quorum: a modality only "speaks" when enough of its windows survived
    // the quality gate. Without it, one usable window out of thirty would be
    // presented as an epoch of evidence.
    const uint16_t quorum = cfg_.min_samples_per_epoch;
    out.imu_usable = acc_.imu_windows >= quorum;
    out.ppg_usable = acc_.ppg_windows >= quorum;

    if (!out.imu_usable) {
        out.quality = DataQuality::Insufficient;  // nothing can be claimed
    } else if (!out.ppg_usable) {
        out.quality = DataQuality::Degraded;  // IMU-only mode (F06 §10)
    } else {
        out.quality = worstQuality(acc_.worst_imu, acc_.worst_ppg);
    }

    const float imu_windows = static_cast<float>(acc_.imu_windows);
    const float ppg_windows = static_cast<float>(acc_.ppg_windows);
    out.enmo_mean_g = imu_windows > 0.0f ? acc_.enmo_sum_g / imu_windows : 0.0f;
    out.enmo_low_ratio =
        imu_windows > 0.0f ? static_cast<float>(acc_.low_motion_windows) / imu_windows : 0.0f;
    out.mag_std_mean_g = imu_windows > 0.0f ? acc_.mag_std_sum_g / imu_windows : 0.0f;
    out.jerk_rms_mean_g_per_s = imu_windows > 0.0f ? acc_.jerk_sum_g_per_s / imu_windows : 0.0f;
    out.stillness_max_s = acc_.stillness_max_s;
    out.hr_mean_bpm =
        acc_.hr_windows > 0 ? acc_.hr_sum_bpm / static_cast<float>(acc_.hr_windows) : 0.0f;
    out.hr_valid_ratio =
        ppg_windows > 0.0f ? static_cast<float>(acc_.hr_windows) / ppg_windows : 0.0f;
    out.activity_confidence =
        acc_.activity_windows > 0
            ? acc_.activity_conf_sum / static_cast<float>(acc_.activity_windows)
            : 0.0f;

    // Most frequent usable window; all-zero (no activity estimate) selects
    // index 0 == ActivityContext::Unknown, which is the honest answer.
    uint8_t dominant = 0;
    for (uint8_t i = 1; i < kActivityContextCount; ++i) {
        if (acc_.activity_hist[i] > acc_.activity_hist[dominant]) {
            dominant = i;
        }
    }
    out.activity_dominant = static_cast<ActivityContext>(dominant);

    // Baseline-relative HR: F13 is optional (F06 §3), and a missing provider
    // must behave exactly like an invalid profile — no HR evidence, not a
    // fabricated z-score.
    out.baseline_valid = false;
    out.hr_z_baseline = 0.0f;
    if (baseline_ != nullptr && acc_.hr_windows > 0) {
        BaselineProfile profile{};
        if (baseline_->profile(profile) && profile.valid) {
            const float mad = profile.resting_hr_mad > cfg_.min_hr_mad_floor_bpm
                                  ? profile.resting_hr_mad
                                  : cfg_.min_hr_mad_floor_bpm;
            out.hr_z_baseline = (out.hr_mean_bpm - profile.resting_hr_bpm) / mad;
            out.baseline_valid = true;
        }
    }

    // Time of day: unixSeconds() == 0 means "no wall clock yet" (02 §2), and
    // a null IClock means the same thing. [ASSUMED] the device's unix seconds
    // are local time — the contract carries no timezone (F06 §13).
    out.tod_valid = false;
    out.tod_min = 0;
    if (clock_ != nullptr) {
        const UnixSeconds unix = clock_->unixSeconds();
        if (unix != 0) {
            out.tod_min = static_cast<uint16_t>((unix % kSecondsPerDay) / kSecondsPerMinute);
            out.tod_valid = true;
        }
    }

    // Sliding window by design: at capacity the OLDEST epoch leaves the 5
    // minutes of context, which is exactly what kHistoryEpochs means — the
    // Overflow code here is the container reporting a normal overwrite, not
    // a fault to propagate (bw/containers/ring_buffer.h).
    static_cast<void>(history_.push(out));
    clearAccumulators();
}

bool FeatureAssembler::step(const WakeSample& sample, EpochSummary& out) {
    // epoch_length_ms == 0 can never complete an epoch (no division, no
    // commit): a nonsensical config degrades to "no estimate", never to a
    // crash or a fabricated window.
    const TimeMs index = cfg_.epoch_length_ms == 0 ? 0 : sample.t_ms / cfg_.epoch_length_ms;

    bool committed = false;
    if (acc_active_ && index != epoch_index_) {
        commit(out);
        committed = true;
    }
    epoch_index_ = index;
    acc_active_ = true;
    accumulate(sample);
    return committed;
}

}  // namespace bw::wake
