// bw_activity — ActivityClassifier tests (F03 §9): table-driven decision
// rows, dwell/hysteresis, quality gating, boundaries and the failure
// behaviour of spec §10. Time is explicit (t_ms on the input), so every
// test is deterministic — no clock, no sleeps.

#include <unity.h>

#include <bw/activity/activity_classifier.h>
#include <bw/context.h>
#include <bw/features.h>

using bw::ActivityContext;
using bw::ActivityEstimate;
using bw::DataQuality;
using bw::MotionFeatures;
using bw::TimeMs;
using bw::activity::ActivityClassifier;
using bw::activity::ActivityConfig;

namespace {

// [DERIVED] The 02 §4.2 contract rate: one MotionFeatures per 1 s window.
constexpr TimeMs kWindowMs = 1000;

// A Good-quality, all-zero window; callers override what the row is about.
MotionFeatures makeFeatures() {
    MotionFeatures f{};
    f.t_ms = kWindowMs;
    f.quality = DataQuality::Good;
    f.enmo_g = 0.0f;
    f.mag_std_g = 0.0f;
    f.jerk_rms_g_per_s = 0.0f;
    f.gyro_rms_dps = 0.0f;
    f.gravity_tilt_deg = 0.0f;
    f.dominant_freq_hz = 0.0f;
    f.stillness_ms = 0;
    return f;
}

// Feeds `windows` copies of `f` at the 1 Hz contract cadence (t_ms =
// 1000, 2000, ...) and returns the last estimate — windows >= the dwell so
// a row's expectation sees the settled output.
ActivityEstimate feed(ActivityClassifier& classifier, MotionFeatures f, int windows) {
    ActivityEstimate out{};
    for (int i = 0; i < windows; ++i) {
        f.t_ms = static_cast<TimeMs>(i + 1) * kWindowMs;
        out = classifier.step(f);
    }
    return out;
}

void assertContext(ActivityContext expected, const ActivityEstimate& actual, const char* msg) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(actual.context),
                                  msg);
}

void assertConfidence(float expected, const ActivityEstimate& actual, const char* msg) {
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, expected, actual.confidence, msg);
}

}  // namespace

void test_classifier_decision_table(void) {
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    stationary.gravity_tilt_deg = 10.0f;

    MotionFeatures degraded_still = makeFeatures();
    degraded_still.quality = DataQuality::Degraded;
    degraded_still.enmo_g = 0.005f;
    degraded_still.gravity_tilt_deg = 10.0f;

    MotionFeatures lying = makeFeatures();
    lying.stillness_ms = 5000;
    lying.gravity_tilt_deg = 90.0f;

    MotionFeatures degraded_lying = makeFeatures();
    degraded_lying.quality = DataQuality::Degraded;
    degraded_lying.enmo_g = 0.005f;
    degraded_lying.gravity_tilt_deg = 90.0f;

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;

    MotionFeatures running = makeFeatures();
    running.dominant_freq_hz = 3.0f;
    running.enmo_g = 0.4f;

    MotionFeatures prolonged = makeFeatures();
    prolonged.stillness_ms = 600000;  // == kProlongedInactivityMs

    MotionFeatures moving_no_cadence = makeFeatures();
    moving_no_cadence.enmo_g = 0.1f;

    MotionFeatures insufficient = makeFeatures();
    insufficient.quality = DataQuality::Insufficient;

    struct Row {
        MotionFeatures features;
        ActivityContext expected;
        float expected_confidence;
        const char* name;
    };
    const Row rows[] = {
        // R1
        {insufficient, ActivityContext::Unknown, 0.0f, "R1 Insufficient => Unknown/0"},
        // R2
        {prolonged, ActivityContext::InactiveProlonged, 0.7f,
         "R2 10 min still => InactiveProlonged"},
        // R3
        {running, ActivityContext::Running, 0.8f, "R3 cadence+impact => Running"},
        // R4
        {walking, ActivityContext::Walking, 0.75f, "R4 cadence+motion => Walking"},
        // R5
        {lying, ActivityContext::LyingResting, 0.5f, "R5 still+horizontal => LyingResting at cap"},
        // R6
        {stationary, ActivityContext::Stationary, 0.7f, "R6 still => Stationary"},
        // R7
        {moving_no_cadence, ActivityContext::Unknown, 0.0f, "R7 moving, no cadence => Unknown/0"},
        // R9/R8 via Degraded factor
        {degraded_still, ActivityContext::Stationary, 0.525f,
         "R9 Degraded Stationary 0.7*0.75 reportable"},
        {degraded_lying, ActivityContext::Unknown, 0.0f, "R9 Degraded posture falls below floor"},
    };

    for (const Row& row : rows) {
        ActivityClassifier classifier;
        const ActivityEstimate out = feed(classifier, row.features, 4);
        assertContext(row.expected, out, row.name);
        assertConfidence(row.expected_confidence, out, row.name);
        // The estimate is stamped with the input's window time (02 §4.3).
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(4 * kWindowMs, out.t_ms, row.name);
    }
}

void test_classifier_first_call_is_unknown(void) {
    // Startup transient: no evidence yet, so the first window is Unknown
    // with confidence 0 — never a guess (00 §7).
    ActivityClassifier classifier;
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    const ActivityEstimate out = classifier.step(stationary);
    assertContext(ActivityContext::Unknown, out, "first call => Unknown");
    assertConfidence(0.0f, out, "first call => confidence 0");
}

void test_classifier_dwell_needs_three_windows(void) {
    ActivityClassifier classifier;
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    feed(classifier, stationary, 4);  // settle into Stationary

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;
    walking.t_ms = 5000;

    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "window 1 of candidate still holds Stationary");
    walking.t_ms = 6000;
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "window 2 of candidate still holds Stationary");
    walking.t_ms = 7000;
    assertContext(ActivityContext::Walking, classifier.step(walking),
                  "window 3 commits the candidate");
}

void test_classifier_dwell_blip_does_not_flap(void) {
    ActivityClassifier classifier;
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    feed(classifier, stationary, 4);

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;
    walking.t_ms = 5000;
    classifier.step(walking);  // candidate #1
    walking.t_ms = 6000;
    classifier.step(walking);  // candidate #2 — a 2-window blip
    stationary.t_ms = 7000;
    assertContext(ActivityContext::Stationary, classifier.step(stationary),
                  "returning to current resets the candidate");

    walking.t_ms = 8000;
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "blip restarts dwell from zero (would flap without reset)");
}

void test_classifier_dwell_gap_resets(void) {
    ActivityClassifier classifier;
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    feed(classifier, stationary, 4);  // last t_ms = 4000

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;
    walking.t_ms = 5000;
    classifier.step(walking);  // dwell 1
    walking.t_ms = 6000;
    classifier.step(walking);  // dwell 2

    // A 5 s jump (windows were lost): dwell evidence is no longer
    // continuous, so the counter restarts — without the reset this window
    // would be the 3rd and commit Walking.
    walking.t_ms = 11000;
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "gap window restarts dwell");
    walking.t_ms = 12000;
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "second post-gap window is dwell 2");
    walking.t_ms = 13000;
    assertContext(ActivityContext::Walking, classifier.step(walking),
                  "third consecutive post-gap window commits");
}

void test_classifier_backwards_timestamp_resets_dwell(void) {
    ActivityClassifier classifier;
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;
    feed(classifier, stationary, 4);  // last t_ms = 4000

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;
    walking.t_ms = 5000;
    classifier.step(walking);  // dwell 1
    walking.t_ms = 6000;
    classifier.step(walking);  // dwell 2

    walking.t_ms = 1000;  // backwards step (reboot/wrap suspicion)
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "backwards timestamp resets dwell, no crash");
    walking.t_ms = 2000;
    assertContext(ActivityContext::Stationary, classifier.step(walking),
                  "recovery restarts dwell at 1");
    walking.t_ms = 3000;
    assertContext(ActivityContext::Walking, classifier.step(walking),
                  "fresh evidence commits after recovery");
}

void test_classifier_insufficient_commits_unknown_immediately(void) {
    ActivityClassifier classifier;
    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;
    assertContext(ActivityContext::Walking, feed(classifier, walking, 4), "settled Walking");

    // One Insufficient window: the stale label must not outlive its data
    // (spec §10) — immediate, no dwell.
    MotionFeatures bad = makeFeatures();
    bad.quality = DataQuality::Insufficient;
    bad.t_ms = 5000;
    const ActivityEstimate out = classifier.step(bad);
    assertContext(ActivityContext::Unknown, out, "Insufficient commits Unknown at once");
    assertConfidence(0.0f, out, "Insufficient carries confidence 0");
    TEST_ASSERT_EQUAL_UINT64(5000, out.t_ms);
}

void test_classifier_prolonged_inactivity_boundary(void) {
    // One millisecond below the threshold stays Stationary ...
    MotionFeatures below = makeFeatures();
    below.stillness_ms = 599999;  // kProlongedInactivityMs - 1
    ActivityClassifier c_below;
    assertContext(ActivityContext::Stationary, feed(c_below, below, 4),
                  "stillness below threshold => Stationary");

    // ... and exactly at it the duration row wins (first match, before
    // posture: a still horizontal arm for 10 min reports the duration).
    MotionFeatures at = makeFeatures();
    at.stillness_ms = 600000;
    at.gravity_tilt_deg = 90.0f;
    ActivityClassifier c_at;
    const ActivityEstimate out = feed(c_at, at, 4);
    assertContext(ActivityContext::InactiveProlonged, out,
                  "stillness at threshold => InactiveProlonged");
    assertConfidence(0.7f, out, "prolonged base confidence");
}

void test_classifier_posture_never_sitting_or_standing(void) {
    // D6: wrist tilt cannot separate Sitting/Standing, so v1 must never
    // emit them — every orientation of a still arm is Stationary or
    // LyingResting (02 §4.3 allows collapsing).
    for (float tilt = 0.0f; tilt <= 180.0f; tilt += 10.0f) {
        MotionFeatures still = makeFeatures();
        still.stillness_ms = 3000;
        still.gravity_tilt_deg = tilt;
        ActivityClassifier classifier;
        const ActivityEstimate out = feed(classifier, still, 4);
        TEST_ASSERT_TRUE_MESSAGE(out.context != ActivityContext::Sitting,
                                 "Sitting must never be emitted in v1");
        TEST_ASSERT_TRUE_MESSAGE(out.context != ActivityContext::Standing,
                                 "Standing must never be emitted in v1");
        TEST_ASSERT_TRUE_MESSAGE(out.context == ActivityContext::Stationary ||
                                     out.context == ActivityContext::LyingResting,
                                 "still arm is Stationary or LyingResting only");
    }
}

void test_classifier_floor_suppresses_low_confidence(void) {
    // R8 in isolation: base 0.6 × Degraded 0.8 = 0.48 < 0.5 => Unknown/0,
    // even though the row itself matched Stationary.
    ActivityConfig config;
    config.conf_stationary = 0.6f;
    config.degraded_confidence_factor = 0.8f;
    ActivityClassifier classifier(config);

    MotionFeatures degraded_still = makeFeatures();
    degraded_still.quality = DataQuality::Degraded;
    degraded_still.enmo_g = 0.005f;
    const ActivityEstimate out = feed(classifier, degraded_still, 4);
    assertContext(ActivityContext::Unknown, out, "confidence below floor => Unknown");
    assertConfidence(0.0f, out, "suppressed claim carries confidence 0");
}

void test_classifier_size_within_budget(void) {
    // Spec §11: RAM < 1 KB for the feature's live state (fixed-size members,
    // no heap); trivial copyability of the payload is F00's contract test.
    TEST_ASSERT_LESS_THAN_MESSAGE(1024, sizeof(ActivityClassifier),
                                  "classifier state fits the 1 KB budget (200 B measured)");
}

void test_classifier_candidate_confidence_refreshed_during_dwell(void) {
    // D5: confidence = base × quality factor, recomputed per window. A
    // quality drop mid-dwell must lower the confidence that gets committed —
    // never the higher value captured when the candidate first appeared.
    //
    // (A) Candidate enters at kConfWalking = 0.75 on a Good window, then
    // two Degraded windows: 0.75 × kDegradedConfidenceFactor (0.75) = 0.5625.
    ActivityClassifier walking_classifier;
    MotionFeatures good_walk = makeFeatures();
    good_walk.dominant_freq_hz = 1.6f;
    good_walk.enmo_g = 0.1f;
    good_walk.t_ms = 1000;
    walking_classifier.step(good_walk);  // candidate Walking @ 0.75 (dwell 1)

    MotionFeatures degraded_walk = good_walk;
    degraded_walk.quality = DataQuality::Degraded;
    degraded_walk.t_ms = 2000;
    walking_classifier.step(degraded_walk);  // dwell 2 @ 0.5625
    degraded_walk.t_ms = 3000;
    const ActivityEstimate committed = walking_classifier.step(degraded_walk);  // dwell 3
    assertContext(ActivityContext::Walking, committed, "candidate commits on window 3");
    assertConfidence(0.5625f, committed,
                     "committed confidence is the latest (Degraded) one, not 0.75");

    // (B) The F02-realistic Degraded path: F02 zeroes stillness_ms and
    // dominant_freq_hz on non-Good windows (verified in bw_signal's
    // ImuWindowProcessor::close), so a Degraded window still classifies via
    // the ENMO branch of `still` — Stationary, 0.7 → 0.7 × 0.75 = 0.525.
    ActivityClassifier stationary_classifier;
    MotionFeatures good_still = makeFeatures();
    good_still.stillness_ms = 3000;
    good_still.gravity_tilt_deg = 10.0f;
    good_still.t_ms = 1000;
    stationary_classifier.step(good_still);  // candidate Stationary @ 0.7 (dwell 1)

    MotionFeatures degraded_still = makeFeatures();
    degraded_still.quality = DataQuality::Degraded;
    degraded_still.enmo_g = 0.005f;
    degraded_still.gravity_tilt_deg = 10.0f;
    degraded_still.t_ms = 2000;
    stationary_classifier.step(degraded_still);  // dwell 2 @ 0.525
    degraded_still.t_ms = 3000;
    const ActivityEstimate committed_still = stationary_classifier.step(degraded_still);
    assertContext(ActivityContext::Stationary, committed_still, "candidate commits on window 3");
    assertConfidence(0.525f, committed_still,
                     "committed confidence follows the Degraded window, not 0.7");
}

void test_classifier_step_gap_boundary(void) {
    // §8 kMaxFeatureStepGapMs (1500, DERIVED): a gap of exactly 1500 ms is
    // still continuous evidence; 1501 ms means windows were lost → restart
    // the dwell (D4).
    MotionFeatures stationary = makeFeatures();
    stationary.stillness_ms = 3000;

    MotionFeatures walking = makeFeatures();
    walking.dominant_freq_hz = 1.6f;
    walking.enmo_g = 0.1f;

    // Exactly 1500 ms: no reset, the 3rd candidate window commits.
    ActivityClassifier at_limit;
    feed(at_limit, stationary, 4);  // t = 1000..4000
    walking.t_ms = 5000;
    at_limit.step(walking);  // dwell 1
    walking.t_ms = 6000;
    at_limit.step(walking);  // dwell 2
    walking.t_ms = 7500;     // 6000 + kMaxFeatureStepGapMs
    const ActivityEstimate continuous = at_limit.step(walking);
    assertContext(ActivityContext::Walking, continuous, "1500 ms gap keeps the dwell");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(7500, continuous.t_ms, "estimate stamped with input time");

    // One millisecond more: the jump resets the dwell, which then needs
    // three fresh windows.
    ActivityClassifier past_limit;
    feed(past_limit, stationary, 4);
    walking.t_ms = 5000;
    past_limit.step(walking);  // dwell 1
    walking.t_ms = 6000;
    past_limit.step(walking);  // dwell 2
    walking.t_ms = 7501;       // 6000 + kMaxFeatureStepGapMs + 1
    assertContext(ActivityContext::Stationary, past_limit.step(walking),
                  "1501 ms gap restarts the dwell");
    walking.t_ms = 8501;
    assertContext(ActivityContext::Stationary, past_limit.step(walking),
                  "post-gap window 2 still holds Stationary");
    walking.t_ms = 9501;
    assertContext(ActivityContext::Walking, past_limit.step(walking), "post-gap window 3 commits");
}

void test_classifier_tilt_band_inclusivity(void) {
    // R5: tilt ∈ [70°, 110°] — both ends inclusive (§8 kLyingTiltMinDeg /
    // kLyingTiltMaxDeg); one step outside collapses to R6 Stationary.
    struct Row {
        float tilt_deg;
        ActivityContext expected;
        float expected_confidence;
        const char* name;
    };
    const Row rows[] = {
        {69.9f, ActivityContext::Stationary, 0.7f, "69.9 deg is below the lying band"},
        {70.0f, ActivityContext::LyingResting, 0.5f, "70.0 deg enters LyingResting (inclusive)"},
        {110.0f, ActivityContext::LyingResting, 0.5f, "110.0 deg still LyingResting (inclusive)"},
        {110.1f, ActivityContext::Stationary, 0.7f, "110.1 deg is above the lying band"},
    };
    for (const Row& row : rows) {
        MotionFeatures still = makeFeatures();
        still.stillness_ms = 3000;
        still.gravity_tilt_deg = row.tilt_deg;
        ActivityClassifier classifier;
        const ActivityEstimate out = feed(classifier, still, 4);
        assertContext(row.expected, out, row.name);
        assertConfidence(row.expected_confidence, out, row.name);
    }
}

void test_classifier_insufficient_beats_duration_and_cadence(void) {
    // R1 is evaluated before every other row: even a window carrying
    // prolonged-inactivity evidence (R2) *and* running cadence + impact
    // (R3) reports Unknown/0 immediately when quality is Insufficient, and
    // never accumulates dwell (spec §10: a stale label must not outlive the
    // data that supported it).
    MotionFeatures bad = makeFeatures();
    bad.quality = DataQuality::Insufficient;
    bad.stillness_ms = 600000;  // kProlongedInactivityMs
    bad.dominant_freq_hz = 3.0f;
    bad.enmo_g = 0.4f;
    bad.gravity_tilt_deg = 90.0f;

    ActivityClassifier classifier;
    const ActivityEstimate first = classifier.step(bad);
    assertContext(ActivityContext::Unknown, first, "R1 wins over R2 and R3");
    assertConfidence(0.0f, first, "R1 carries confidence 0");

    bad.t_ms = 2000;
    classifier.step(bad);
    bad.t_ms = 3000;
    const ActivityEstimate later = classifier.step(bad);
    assertContext(ActivityContext::Unknown, later, "repeated Insufficient never dwells");
    assertConfidence(0.0f, later, "still Unknown/0 after three Insufficient windows");
}
