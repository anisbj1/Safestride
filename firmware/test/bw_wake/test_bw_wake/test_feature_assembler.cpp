// test/bw_wake/test_bw_wake/test_feature_assembler.cpp — F06 D2/D4: gating,
// epoching, history, baseline/clock handling and the schema vector.

#include <unity.h>

#include "wake_test_support.h"

using namespace bw;
using namespace bw::wake;
using namespace bw::wake::test;

namespace {

// Feeds `count` windows at 1 s spacing starting at `t0`. Returns true when
// the LAST window crossed an epoch boundary (i.e. `out` holds that epoch).
bool feed(FeatureAssembler& assembler, TimeMs t0, uint16_t count, float enmo, EpochSummary& out) {
    bool committed = false;
    for (uint16_t i = 0; i < count; ++i) {
        committed = assembler.step(makeSample(t0 + static_cast<TimeMs>(i) * 1000u, enmo), out);
    }
    return committed;
}

}  // namespace

void test_assembler_first_call_accumulates(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    TEST_ASSERT_FALSE(assembler.step(makeSample(0, 0.05f), out));
    TEST_ASSERT_EQUAL_UINT16(0, assembler.history().size());
}

void test_assembler_commits_at_epoch_boundary(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    TEST_ASSERT_FALSE(feed(assembler, 0, 30, 0.05f, out));  // windows 0..29 s
    // The first window of the NEXT epoch is what commits the previous one.
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_EQUAL_UINT16(30, out.sample_count);
    TEST_ASSERT_EQUAL_UINT64(29000, out.t_ms);  // last window folded in
    TEST_ASSERT_EQUAL_UINT16(1, assembler.history().size());
}

void test_assembler_means_and_low_motion_ratio(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 30; ++i) {
        static_cast<void>(assembler.step(makeSample(i * 1000u, i < 10 ? 0.001f : 0.1f), out));
    }
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.1f), out));

    // 10 windows at 0.001 g + 20 at 0.1 g over 30 windows.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.067f, out.enmo_mean_g);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f / 30.0f, out.enmo_low_ratio);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out.stillness_max_s);  // 1 s of stillness
}

void test_assembler_insufficient_imu_invalidates_epoch(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 30; ++i) {
        static_cast<void>(
            assembler.step(makeSample(i * 1000u, 0.05f, DataQuality::Insufficient), out));
    }
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_FALSE(out.imu_usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.quality));
}

void test_assembler_missing_ppg_degrades_but_keeps_estimate(void) {
    // F06 §10: PPG unavailable => IMU-only feature set, flagged — not an
    // invalid epoch.
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 30; ++i) {
        static_cast<void>(assembler.step(makeSampleNoPpg(i * 1000u, 0.05f), out));
    }
    TEST_ASSERT_TRUE(assembler.step(makeSampleNoPpg(30000, 0.05f), out));
    TEST_ASSERT_TRUE(out.imu_usable);
    TEST_ASSERT_FALSE(out.ppg_usable);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Degraded), static_cast<int>(out.quality));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.hr_valid_ratio);
}

void test_assembler_modality_quorum(void) {
    const WakeConfig cfg{};
    {  // 20 usable of 30: above kMinSamplesPerEpoch (15) -> the modality speaks
        FeatureAssembler assembler{cfg, nullptr, nullptr};
        EpochSummary out{};
        for (uint16_t i = 0; i < 30; ++i) {
            const DataQuality q = i < 20 ? DataQuality::Good : DataQuality::Insufficient;
            static_cast<void>(assembler.step(makeSample(i * 1000u, 0.05f, q), out));
        }
        TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
        TEST_ASSERT_TRUE(out.imu_usable);
    }
    {  // 10 usable of 30: below the quorum -> no estimate
        FeatureAssembler assembler{cfg, nullptr, nullptr};
        EpochSummary out{};
        for (uint16_t i = 0; i < 30; ++i) {
            const DataQuality q = i < 10 ? DataQuality::Good : DataQuality::Insufficient;
            static_cast<void>(assembler.step(makeSample(i * 1000u, 0.05f, q), out));
        }
        TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
        TEST_ASSERT_FALSE(out.imu_usable);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                              static_cast<int>(out.quality));
    }
}

void test_assembler_baseline_absent_is_neutral(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};  // no provider at all
    EpochSummary out{};
    TEST_ASSERT_FALSE(feed(assembler, 0, 30, 0.05f, out));
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_FALSE(out.baseline_valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.hr_z_baseline);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 70.0f, out.hr_mean_bpm);
}

void test_assembler_baseline_z_score(void) {
    const WakeConfig cfg{};
    FakeBaseline baseline{};
    baseline.available = true;
    baseline.p.valid = true;
    baseline.p.resting_hr_bpm = 60.0f;
    baseline.p.resting_hr_mad = 5.0f;

    FeatureAssembler assembler{cfg, nullptr, &baseline};
    EpochSummary out{};
    TEST_ASSERT_FALSE(feed(assembler, 0, 30, 0.05f, out));
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_TRUE(out.baseline_valid);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, (70.0f - 60.0f) / 5.0f, out.hr_z_baseline);
}

void test_assembler_baseline_mad_floor(void) {
    // A profile with no MAD must not divide by zero (kMinHrMadFloorBpm).
    const WakeConfig cfg{};
    FakeBaseline baseline{};
    baseline.available = true;
    baseline.p.valid = true;
    baseline.p.resting_hr_bpm = 60.0f;
    baseline.p.resting_hr_mad = 0.0f;

    FeatureAssembler assembler{cfg, nullptr, &baseline};
    EpochSummary out{};
    TEST_ASSERT_FALSE(feed(assembler, 0, 30, 0.05f, out));
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_TRUE(out.baseline_valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, (70.0f - 60.0f) / kMinHrMadFloorBpm, out.hr_z_baseline);
}

void test_assembler_time_of_day(void) {
    const WakeConfig cfg{};
    testkit::FakeClock clock{};
    FeatureAssembler assembler{cfg, &clock, nullptr};
    EpochSummary out{};

    TEST_ASSERT_FALSE(feed(assembler, 0, 30, 0.05f, out));
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_FALSE(out.tod_valid);  // unixSeconds() == 0 means unknown (02 §2)

    clock.setUnixSeconds(14u * 3600u + 30u * 60u);  // 14:30 local
    // Windows 31 s..60 s: the last one crosses into epoch 2 and commits
    // epoch 1 with the clock already set.
    TEST_ASSERT_TRUE(feed(assembler, 31000, 30, 0.05f, out));
    TEST_ASSERT_TRUE(out.tod_valid);
    TEST_ASSERT_EQUAL_UINT16(870, out.tod_min);
}

void test_assembler_backwards_timestamp_commits_and_restarts(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    TEST_ASSERT_FALSE(assembler.step(makeSample(35000, 0.05f), out));
    TEST_ASSERT_FALSE(assembler.step(makeSample(40000, 0.05f), out));
    // Reboot boundary (01 §4): time jumps backwards across an epoch edge.
    TEST_ASSERT_TRUE(assembler.step(makeSample(5000, 0.05f), out));
    TEST_ASSERT_EQUAL_UINT16(2, out.sample_count);
    TEST_ASSERT_EQUAL_UINT64(40000, out.t_ms);
    TEST_ASSERT_EQUAL_UINT16(1, assembler.history().size());
}

void test_assembler_zero_epoch_length_never_commits(void) {
    WakeConfig cfg{};
    cfg.epoch_length_ms = 0;  // nonsense config: no commit, no crash (F06 §10)
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 100; ++i) {
        TEST_ASSERT_FALSE(assembler.step(makeSample(i * 1000u, 0.05f), out));
    }
    TEST_ASSERT_EQUAL_UINT16(0, assembler.history().size());
}

void test_assembler_history_is_bounded(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint32_t i = 0; i < 480; ++i) {  // 16 epochs of 30 windows
        static_cast<void>(assembler.step(makeSample(i * 1000u, 0.05f), out));
    }
    // 15 commits into a 10-slot ring: full, with 5 documented overwrites
    // (the sliding window's normal behaviour, not a fault).
    TEST_ASSERT_EQUAL_UINT16(kHistoryEpochs, assembler.history().size());
    TEST_ASSERT_EQUAL_UINT32(5, assembler.history().overwrites());
}

void test_assembler_dominant_activity(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 30; ++i) {
        WakeSample s = makeSample(i * 1000u, 0.05f);
        s.activity.context = i < 20 ? ActivityContext::Sitting : ActivityContext::Walking;
        s.activity.confidence = 0.8f;
        static_cast<void>(assembler.step(s, out));
    }
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityContext::Sitting),
                          static_cast<int>(out.activity_dominant));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.8f, out.activity_confidence);
}

void test_assembler_low_confidence_activity_is_ignored(void) {
    const WakeConfig cfg{};
    FeatureAssembler assembler{cfg, nullptr, nullptr};
    EpochSummary out{};
    for (uint16_t i = 0; i < 30; ++i) {
        WakeSample s = makeSample(i * 1000u, 0.05f);
        s.activity.context = ActivityContext::LyingResting;
        s.activity.confidence = 0.2f;  // below kMinActivityConfidence
        static_cast<void>(assembler.step(s, out));
    }
    TEST_ASSERT_TRUE(assembler.step(makeSample(30000, 0.05f), out));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ActivityContext::Unknown),
                          static_cast<int>(out.activity_dominant));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.activity_confidence);
}

void test_build_vector_normalization_and_schema_stamp(void) {
    const WakeConfig cfg{};
    EpochSummary e = makeEpoch();
    e.enmo_mean_g = 0.15f;
    EpochHistory history{};
    pushHistory(history, e);

    FeatureVector v{};
    const Result<void> built = buildFeatureVector(cfg, e, history, e.t_ms, v);
    TEST_ASSERT_TRUE(built.ok());
    TEST_ASSERT_EQUAL_UINT16(kAlertnessSchemaId, v.schema_id);
    TEST_ASSERT_EQUAL_UINT16(kAlertnessSchemaVersion, v.schema_version);
    TEST_ASSERT_EQUAL_UINT8(kAlertnessFeatureCount, v.count);
    TEST_ASSERT_EQUAL_UINT64(e.t_ms, v.t_ms);

    // (0.15 - 0.05) / 0.10 == 1.0 — FEATURE_SCHEMA §4's single formula.
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, v.v[0]);
    // "none" normalization is the identity: the raw value passes through.
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, e.hr_valid_ratio, v.v[6]);
}

void test_build_vector_missing_inputs_land_on_identity(void) {
    const WakeConfig cfg{};
    EpochSummary e = makeEpoch();
    e.hr_valid_ratio = 0.0f;
    e.hr_mean_bpm = 0.0f;
    e.baseline_valid = false;
    e.tod_valid = false;
    EpochHistory history{};
    pushHistory(history, e);

    FeatureVector v{};
    TEST_ASSERT_TRUE(buildFeatureVector(cfg, e, history, e.t_ms, v).ok());
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, v.v[5]);   // hr_bpm -> schema mean
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, v.v[8]);   // baseline_valid
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, v.v[13]);  // tod_valid
}

void test_build_vector_time_of_day_is_circular(void) {
    const WakeConfig cfg{};
    EpochSummary e = makeEpoch();
    e.tod_valid = true;
    e.tod_min = 870;  // 14:30
    EpochHistory history{};
    pushHistory(history, e);

    FeatureVector v{};
    TEST_ASSERT_TRUE(buildFeatureVector(cfg, e, history, e.t_ms, v).ok());
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, v.v[13]);
    // sin^2 + cos^2 == 1: a point on the day circle, so 23:59 and 00:01 stay
    // adjacent instead of 1439 and 0 apart.
    const float norm = v.v[11] * v.v[11] + v.v[12] * v.v[12];
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, norm);
}

void test_build_vector_rejects_empty_epoch(void) {
    const WakeConfig cfg{};
    EpochSummary e = makeEpoch();
    e.sample_count = 0;
    EpochHistory history{};
    FeatureVector v{};
    const Result<void> built = buildFeatureVector(cfg, e, history, e.t_ms, v);
    TEST_ASSERT_FALSE(built.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::NotReady), static_cast<int>(built.err));
}

void test_history_previous_epoch_invariant(void) {
    EpochHistory history{};
    TEST_ASSERT_TRUE(previousEpoch(history) == nullptr);

    const EpochSummary previous = makeEpoch(20000);
    const EpochSummary current = makeEpoch(50000);
    pushHistory(history, previous);
    pushHistory(history, current);
    // history()'s newest slot IS the current epoch (feature_assembler.h).
    const EpochSummary* prev = previousEpoch(history);
    TEST_ASSERT_TRUE(prev != nullptr);
    TEST_ASSERT_EQUAL_UINT64(20000, prev->t_ms);
}

void test_low_motion_streak_counts_and_breaks(void) {
    const WakeConfig cfg{};
    EpochHistory history{};

    EpochSummary current = makeEpoch();
    current.enmo_low_ratio = 0.9f;
    pushHistory(history, current);
    TEST_ASSERT_EQUAL_UINT16(1, lowMotionEpochStreak(current, history, cfg));

    EpochSummary second = makeEpoch(60000);
    second.enmo_low_ratio = 0.8f;
    pushHistory(history, second);
    TEST_ASSERT_EQUAL_UINT16(2, lowMotionEpochStreak(second, history, cfg));

    // A restless epoch breaks the run even if the older ones were still.
    EpochSummary busy = makeEpoch(90000);
    busy.enmo_low_ratio = 0.1f;
    pushHistory(history, busy);
    TEST_ASSERT_EQUAL_UINT16(0, lowMotionEpochStreak(busy, history, cfg));

    // The streak is capped by the configured duration, never unbounded.
    EpochHistory long_history{};
    for (int i = 0; i < kHistoryEpochs; ++i) {
        EpochSummary still = makeEpoch(120000 + static_cast<TimeMs>(i) * 30000u);
        still.enmo_low_ratio = 0.9f;
        pushHistory(long_history, still);
    }
    const EpochSummary* newest = long_history.at(long_history.size() - 1);
    TEST_ASSERT_TRUE(newest != nullptr);
    TEST_ASSERT_EQUAL_UINT16(cfg.sleep_low_motion_epochs,
                             lowMotionEpochStreak(*newest, long_history, cfg));
}

void test_low_motion_streak_breaks_on_unusable_epoch(void) {
    // A sparse epoch (below kMinSamplesPerEpoch, imu_usable == false) must
    // not extend a low-motion streak: it carries too little data to be
    // duration evidence, and a gap in the stream is not a night of sleep.
    const WakeConfig cfg{};
    EpochHistory history{};

    EpochSummary first = makeEpoch(30000);
    first.enmo_low_ratio = 1.0f;
    pushHistory(history, first);

    EpochSummary sparse = makeEpoch(60000);
    sparse.sample_count = 1;       // one window actually arrived
    sparse.imu_usable = false;     // below the quorum
    sparse.enmo_low_ratio = 1.0f;  // looks low-motion, but has no quorum
    pushHistory(history, sparse);

    EpochSummary current = makeEpoch(90000);
    current.enmo_low_ratio = 1.0f;
    pushHistory(history, current);
    // Without the guard the walk counts first + sparse + current = 3.
    TEST_ASSERT_EQUAL_UINT16(1, lowMotionEpochStreak(current, history, cfg));

    // The current epoch itself unusable: no duration claim at all (0).
    EpochSummary unusable_current = makeEpoch(120000);
    unusable_current.sample_count = 2;
    unusable_current.imu_usable = false;
    unusable_current.enmo_low_ratio = 1.0f;
    pushHistory(history, unusable_current);
    TEST_ASSERT_EQUAL_UINT16(0, lowMotionEpochStreak(unusable_current, history, cfg));
}

void test_enmo_drop_ratio(void) {
    EpochHistory history{};

    const EpochSummary still = makeEpoch(30000);
    pushHistory(history, still);
    // No previous epoch: no trend, never a fabricated spike.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, enmoDropRatio(still, history));

    EpochSummary previous = makeEpoch(60000);
    previous.enmo_mean_g = 0.10f;
    pushHistory(history, previous);

    EpochSummary halved = makeEpoch(90000);
    halved.enmo_mean_g = 0.05f;
    pushHistory(history, halved);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.5f, enmoDropRatio(halved, history));

    EpochSummary risen = makeEpoch(120000);
    risen.enmo_mean_g = 0.20f;
    pushHistory(history, risen);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, enmoDropRatio(risen, history));  // clamped at 0
}
