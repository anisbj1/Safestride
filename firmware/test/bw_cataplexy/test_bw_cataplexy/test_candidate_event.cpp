// firmware/test/bw_cataplexy/test_bw_cataplexy/test_candidate_event.cpp —
// F09 §3: what F09 puts in the Event it publishes, and what it must leave alone.

#include <unity.h>

#include <bw/cataplexy/candidate_detector.h>
#include <bw/cataplexy/candidate_event.h>
#include <bw/cataplexy/cataplexy_config.h>

#include "fixtures.h"

namespace {

using bw::cataplexy::CandidateOutput;
using bw::cataplexy::Confounder;
using bw::cataplexy::confounderBit;
using bw::cataplexy::EventCandidateDetector;
using bw::cataplexy::kCataplexyDetectorId;
using bw::cataplexy::kPayloadConfounderIndex;
using bw::cataplexy::kPayloadWindowFlagIndex;
using bw::cataplexy::kWindowRequestFlag;
using bw::cataplexy::makeSuspectedCataplexyEvent;
using bw::cataplexy::reasonBit;
using bw::cataplexy::testing::DropScenario;
using bw::cataplexy::testing::runDrop;

// A raised candidate straight from the detector, not a hand-built one: the
// event is checked against the real rule output.
CandidateOutput raisedCandidate() {
    EventCandidateDetector detector;
    return runDrop(detector, DropScenario::Clean);
}

}  // namespace

void test_event_type_and_schema(void) {
    const bw::Event event = makeSuspectedCataplexyEvent(raisedCandidate());

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EventType::SuspectedCataplexy),
                            static_cast<uint8_t>(event.type));
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, event.schema_version);
    TEST_ASSERT_EQUAL_UINT8(kCataplexyDetectorId, event.detector_id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Unlabeled),
                            static_cast<uint8_t>(event.label));
}

void test_event_requests_the_raw_window(void) {
    const bw::Event event = makeSuspectedCataplexyEvent(raisedCandidate());

    TEST_ASSERT_BITS_HIGH(kWindowRequestFlag, event.payload[kPayloadWindowFlagIndex]);
}

void test_event_carries_confounder_flags(void) {
    CandidateOutput candidate = raisedCandidate();
    candidate.confounders = static_cast<bw::cataplexy::ConfounderMask>(
        confounderBit(Confounder::PostureChange) | confounderBit(Confounder::BriefBurst));

    const bw::Event event = makeSuspectedCataplexyEvent(candidate);

    TEST_ASSERT_EQUAL_UINT8(confounderBit(Confounder::PostureChange) |
                                confounderBit(Confounder::BriefBurst),
                            event.payload[kPayloadConfounderIndex]);
}

void test_event_reserved_payload_is_zero(void) {
    const bw::Event event = makeSuspectedCataplexyEvent(raisedCandidate());

    for (size_t i = kPayloadConfounderIndex + 1; i < bw::Event::kMaxPayloadBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, event.payload[i]);
    }
}

void test_event_identity_is_left_to_f17(void) {
    const bw::Event event = makeSuspectedCataplexyEvent(raisedCandidate());

    // seq and boot_id are assigned by F17, t_start_unix is unknown until sync
    // (02 §1: 0 == unknown, never a magic value).
    TEST_ASSERT_EQUAL_UINT32(0, event.seq);
    TEST_ASSERT_EQUAL_UINT32(0, event.boot_id);
    TEST_ASSERT_EQUAL_UINT32(0, event.t_start_unix);
}

void test_event_confidence_and_reasons_travel_together(void) {
    const CandidateOutput candidate = raisedCandidate();
    const bw::Event event = makeSuspectedCataplexyEvent(candidate);

    TEST_ASSERT_EQUAL_FLOAT(candidate.confidence, event.confidence);
    TEST_ASSERT_EQUAL_UINT32(candidate.reasons, event.reasons);
    TEST_ASSERT_TRUE(event.reasons != 0);  // §9: confidence never without reasons
    TEST_ASSERT_TRUE(event.confidence > 0.0f);
}

void test_event_span_and_context_are_copied(void) {
    const CandidateOutput candidate = raisedCandidate();
    const bw::Event event = makeSuspectedCataplexyEvent(candidate);

    TEST_ASSERT_EQUAL_UINT64(candidate.t_start_ms, event.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(candidate.duration_ms, event.duration_ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(candidate.context),
                            static_cast<uint8_t>(event.context));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(candidate.alertness_at_start),
                            static_cast<uint8_t>(event.alertness_at_start));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(candidate.quality.imu),
                            static_cast<uint8_t>(event.quality.imu));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(candidate.quality.ppg),
                            static_cast<uint8_t>(event.quality.ppg));
}

void test_event_stays_within_the_fixed_record(void) {
    // 02 §4.5 pins the record at 48 bytes; building one must not change that.
    TEST_ASSERT_EQUAL_UINT32(48, sizeof(bw::Event));
}
