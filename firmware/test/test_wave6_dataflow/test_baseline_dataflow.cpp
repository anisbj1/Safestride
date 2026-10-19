#include "../../src/app/wiring/wire_baseline.cpp"
#include "../../src/app/wiring/wire_wake.cpp"

#include <bw/testkit/fake_clock.h>
#include <bw/testkit/mem_kv_store.h>
#include <bw/wake/feature_assembler.h>
#include <unity.h>

namespace {
// [EXPERIMENTAL] Deterministic resting fixture, distinct from the observed HR.
constexpr float kRestingHrBpm = 72.0f;
constexpr float kObservedHrBpm = 64.0f;
constexpr float kRestingMadBpm = 4.0f;
constexpr float kRestingEnmoG = 0.01f;
// [DERIVED] Float arithmetic tolerance for the baseline z-score comparison.
constexpr float kBaselineTolerance = 0.0001f;

bw::wake::WakeSample sampleAt(bw::TimeMs now) {
    bw::wake::WakeSample s{};
    s.t_ms = now;
    s.motion.t_ms = s.ppg.t_ms = s.activity.t_ms = now;
    s.motion.quality = s.ppg.quality = bw::DataQuality::Good;
    s.quality = {bw::DataQuality::Good, bw::DataQuality::Good};
    s.motion.enmo_g = kRestingEnmoG;
    s.ppg.hr_valid = true;
    s.ppg.hr_bpm = kObservedHrBpm;
    return s;
}

bw::wake::EpochSummary firstEpoch(const bw::IBaselineProvider* baseline) {
    bw::wake::FeatureAssembler assembler{bw::wake::WakeConfig{}, nullptr, baseline};
    bw::wake::EpochSummary epoch{};
    for (bw::TimeMs t = 0; t <= bw::wake::kEpochLengthMs; t += bw::wake::kFeatureWindowMs) {
        const bool committed = assembler.step(sampleAt(t), epoch);
        TEST_ASSERT_EQUAL(t == bw::wake::kEpochLengthMs, committed);
    }
    return epoch;
}
}  // namespace

void test_baseline_absent_and_untrained_do_not_fabricate_hr_evidence() {
    app::AppContext ctx{};
    f13::wireWithStore(ctx, nullptr);
    const auto absent = firstEpoch(nullptr);
    const auto untrained = firstEpoch(&f13::provider());
    TEST_ASSERT_TRUE(absent.ppg_usable);
    TEST_ASSERT_FALSE(absent.baseline_valid);
    TEST_ASSERT_FALSE(untrained.baseline_valid);
    TEST_ASSERT_EQUAL_FLOAT(0, absent.hr_z_baseline);
    TEST_ASSERT_EQUAL_FLOAT(0, untrained.hr_z_baseline);
}

void test_restored_baseline_reaches_actual_f06_injected_estimator() {
    app::AppContext ctx{};
    // Static dependencies match the actual task-lifetime wiring contract.
    static bw::testkit::FakeClock clock;
    static bw::testkit::InMemoryKeyValueStore<> store;
    ctx.clock = &clock;
    const bw::baseline::TrackerState saved{kRestingHrBpm, kRestingMadBpm, kRestingEnmoG,
                                           bw::baseline::kMinQualifiedMs /
                                               bw::baseline::kMsPerSecond};
    TEST_ASSERT_TRUE(bw::baseline::BaselineSerializer::saveToStore(&store, saved).ok());
    f13::wireWithStore(ctx, &store);
    bw::BaselineProfile restored{};
    TEST_ASSERT_TRUE(f13::provider().profile(restored));
    TEST_ASSERT_TRUE(restored.valid);
    f06::wire(ctx, &f13::provider());  // same call-once injection as app.cpp
    bw::wake::WakeEstimate estimate{};
    for (bw::TimeMs t = 0; t <= bw::wake::kEpochLengthMs; t += bw::wake::kFeatureWindowMs) {
        clock.setMs(t);
        estimate = f06::estimator().step(sampleAt(t));
    }
    TEST_ASSERT_TRUE(estimate.probs.valid);
    const auto& history = f06::estimator().assembler().history();
    TEST_ASSERT_EQUAL_UINT32(1, history.size());
    const auto* epoch = history.at(0);
    TEST_ASSERT_NOT_NULL(epoch);
    TEST_ASSERT_TRUE(epoch->baseline_valid);
    TEST_ASSERT_FLOAT_WITHIN(kBaselineTolerance,
                             (kObservedHrBpm - restored.resting_hr_bpm) / restored.resting_hr_mad,
                             epoch->hr_z_baseline);
    TEST_ASSERT_BITS_HIGH(1u << static_cast<uint8_t>(bw::ReasonCode::HrBelowBaseline),
                          estimate.reasons);
}
