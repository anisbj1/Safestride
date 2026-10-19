// test/bw_wake/test_bw_wake/test_model_estimator.cpp — the M5 path: IModel
// glue, schema handshake, contribution -> ReasonCode mapping (F06 §7, §9).

#include <unity.h>

#include "wake_test_support.h"

using namespace bw;
using namespace bw::wake;
using namespace bw::wake::test;

namespace {

bool hasReason(ReasonMask mask, ReasonCode code) {
    return (mask & (static_cast<ReasonMask>(1) << static_cast<uint8_t>(code))) != 0;
}

EpochHistory historyFor(const EpochSummary& epoch) {
    EpochHistory history{};
    pushHistory(history, epoch);
    return history;
}

// A healthy 3-class alertness answer: [awake, drowsy, sleep].
void setThreeClassOutput(FakeModel& model, float awake, float drowsy, float sleep,
                         float confidence) {
    model.output.n_classes = 3;
    model.output.p[0] = awake;
    model.output.p[1] = drowsy;
    model.output.p[2] = sleep;
    model.output.p[3] = 0.0f;
    model.output.confidence = confidence;
}

}  // namespace

void test_model_absent_reports_no_estimate(void) {
    const WakeConfig cfg{};
    const ModelAlertnessEstimator estimator{nullptr, cfg};
    const EpochSummary e = makeEpoch();
    const EpochHistory history = historyFor(e);

    const WakeEstimate out = estimator.estimate(e, history);
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.drowsy);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Good),
                          static_cast<int>(out.probs.input_quality));
}

void test_model_predict_maps_classes_and_stamps_schema(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.2f, 0.7f, 0.1f, 0.9f);
    const ModelAlertnessEstimator estimator{&model, cfg};

    const EpochSummary e = makeEpoch();
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.2f, out.probs.awake);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.7f, out.probs.drowsy);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.1f, out.probs.sleep);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f,
                             out.probs.awake + out.probs.drowsy + out.probs.sleep);

    // The assembler stamped the schema the model is validated against
    // (FEATURE_SCHEMA §3): id, version and count, in that order.
    TEST_ASSERT_EQUAL_UINT16(kAlertnessSchemaId, model.seen.schema_id);
    TEST_ASSERT_EQUAL_UINT16(kAlertnessSchemaVersion, model.seen.schema_version);
    TEST_ASSERT_EQUAL_UINT8(kAlertnessFeatureCount, model.seen.count);
    TEST_ASSERT_EQUAL_UINT32(1, model.predict_calls);
}

void test_model_wrong_class_count_is_rejected(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    model.output.n_classes = 2;  // a detector, not the alertness model
    model.output.p[0] = 0.5f;
    model.output.p[1] = 0.5f;
    model.output.confidence = 0.9f;
    const ModelAlertnessEstimator estimator{&model, cfg};

    const EpochSummary e = makeEpoch();
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_FALSE(out.probs.valid);  // caller falls back to the heuristic
}

void test_model_predict_error_reports_no_estimate(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    model.predict_err = Err::InvalidArg;  // schema mismatch, per FEATURE_SCHEMA §3
    const ModelAlertnessEstimator estimator{&model, cfg};

    const EpochSummary e = makeEpoch();
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.awake);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.probs.confidence);
}

void test_model_skips_prediction_on_unusable_epoch(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.0f, 1.0f, 0.0f, 1.0f);
    const ModelAlertnessEstimator estimator{&model, cfg};

    EpochSummary e = makeEpoch();
    e.imu_usable = false;
    e.quality = DataQuality::Insufficient;
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_UINT32(0, model.predict_calls);  // no query from bad data
}

// Isolates the quality clause of the F06 §8 gate: quorum met (imu_usable
// stays true) yet the epoch's worst window quality is Insufficient, so no
// prediction may be queried from it. The unusable-epoch test above cannot
// catch a regression here — it satisfies the gate with imu_usable alone.
void test_model_insufficient_quality_skips_prediction(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.0f, 1.0f, 0.0f, 1.0f);
    const ModelAlertnessEstimator estimator{&model, cfg};

    EpochSummary e = makeEpoch();
    TEST_ASSERT_TRUE(e.imu_usable);  // precondition: only quality is bad
    e.quality = DataQuality::Insufficient;
    const WakeEstimate out = estimator.estimate(e, historyFor(e));

    TEST_ASSERT_FALSE(out.probs.valid);
    TEST_ASSERT_EQUAL_UINT32(0, model.predict_calls);  // no query from bad data
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DataQuality::Insufficient),
                          static_cast<int>(out.probs.input_quality));
}

void test_model_contributions_map_to_reason_codes(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.1f, 0.8f, 0.1f, 0.95f);
    model.contributions_err = Err::Ok;
    model.contribution_count = 3;
    model.contribution_list[0] = Contribution{0, 1.5f};   // enmo_mean_g
    model.contribution_list[1] = Contribution{8, 2.0f};   // baseline_valid: no code
    model.contribution_list[2] = Contribution{15, 0.5f};  // enmo_drop_ratio
    const ModelAlertnessEstimator estimator{&model, cfg};

    const EpochSummary e = makeEpoch();  // Good quality: no quality reasons
    const WakeEstimate out = estimator.estimate(e, historyFor(e));

    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::SustainedLowMotion));
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::AbruptMotionDrop));
    // A feature with no reason of its own never invents one, and quality
    // reasons are not silently added to a model's explanation.
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::HrBelowBaseline));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::LowSignalQuality));
}

void test_model_small_contributions_are_not_quoted(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.1f, 0.8f, 0.1f, 0.95f);
    model.contributions_err = Err::Ok;
    model.contribution_count = 2;
    model.contribution_list[0] = Contribution{0, 0.05f};  // below the floor
    model.contribution_list[1] = Contribution{5, 0.05f};  // below the floor
    const ModelAlertnessEstimator estimator{&model, cfg};

    const EpochSummary e = makeEpoch();
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.reasons);
}

void test_model_unsupported_contributions_keep_quality_reasons(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.1f, 0.8f, 0.1f, 0.95f);
    model.contributions_err = Err::Unsupported;  // legal per bw/model.h
    const ModelAlertnessEstimator estimator{&model, cfg};

    EpochSummary e = makeEpoch();
    e.quality = DataQuality::Degraded;
    e.ppg_usable = false;
    const WakeEstimate out = estimator.estimate(e, historyFor(e));
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::LowSignalQuality));
    TEST_ASSERT_TRUE(hasReason(out.reasons, ReasonCode::SensorUnavailable));
    TEST_ASSERT_TRUE(!hasReason(out.reasons, ReasonCode::SustainedLowMotion));
}

void test_model_confidence_is_capped_by_data_quality(void) {
    const WakeConfig cfg{};
    FakeModel model{};
    setThreeClassOutput(model, 0.1f, 0.8f, 0.1f, 0.9f);
    const ModelAlertnessEstimator estimator{&model, cfg};

    EpochSummary degraded = makeEpoch();
    degraded.quality = DataQuality::Degraded;
    const WakeEstimate out = estimator.estimate(degraded, historyFor(degraded));
    TEST_ASSERT_TRUE(out.probs.valid);
    TEST_ASSERT_TRUE(out.probs.confidence < model.output.confidence);  // capped
    TEST_ASSERT_TRUE(out.probs.confidence > 0.0f);

    // A model that knows less than the data allows is reported as it is.
    setThreeClassOutput(model, 0.1f, 0.8f, 0.1f, 0.3f);
    const WakeEstimate modest = estimator.estimate(degraded, historyFor(degraded));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.3f, modest.probs.confidence);
}
