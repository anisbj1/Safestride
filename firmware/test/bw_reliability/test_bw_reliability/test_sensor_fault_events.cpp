// bw_reliability — SensorFault event tests (spec §16.8 item 7, plan D13):
// one event per facility ENTER edge with detector_id 26 and the pinned
// payload layout, the per-minute rate limit, re-edges after recovery, the
// one-shot anomalous-boot event and the dropped-publish counter (backlog 18
// CR request).
//
// wire_reliability.cpp is included by test_wire_reliability_compile.cpp only
// (the one-TU rule); this TU links against f26:: through wire_reliability.h.

#include "../../../src/app/wiring/wire_reliability.h"

#include <unity.h>

#include <bw/events.h>
#include <bw/reliability/degradation_policy.h>
#include <bw/reliability/reliability_config.h>

#include "reliability_test_support.h"

namespace {

using reliability_test::FakeEventSink;
using reliability_test::FakeStorageHealth;

constexpr uint32_t codeBit(const bw::FaultCode code) {
    return 1u << static_cast<uint8_t>(code);
}

// Detector id 26 and the payload pin (D13(i)): payload[0] = triggering
// FaultCode, payload[1] = facility bit mask.
void assertFaultEvent(const bw::Event& e, const bw::FaultCode code, const uint8_t facility_bit,
                      const bw::TimeMs t_ms) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::SensorFault), static_cast<int>(e.type));
    TEST_ASSERT_EQUAL_UINT8(bw::reliability::kFaultDetectorId, e.detector_id);
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, e.schema_version);
    TEST_ASSERT_EQUAL_UINT64(t_ms, e.t_start_ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(code), e.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(facility_bit, e.payload[1]);
}

}  // namespace

// Exactly one ENTER edge => exactly one SensorFault (D13(i)): the latched
// PpgAbsent code carries the event, detector 26, payload pinned.
void test_events_enter_edge_emits_one_fault_event(void) {
    FakeEventSink sink;
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 500);

    f26::runTick(1000);
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);
    assertFaultEvent(
        sink.events[0], bw::FaultCode::PpgAbsent,
        static_cast<uint8_t>(bw::reliability::facilityBit(bw::DegradedFacility::PpgLost)), 1000);
    TEST_ASSERT_EQUAL_UINT32(0, f26::droppedEvents());
}

// The flag is held, not flapping: no second event without an exit + re-entry
// (only ENTER edges exist — D13(i)).
void test_events_no_repeat_while_flag_held(void) {
    FakeEventSink sink;
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 500);

    f26::runTick(1000);
    f26::runTick(2000);
    f26::runTick(3000);
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);
}

// D13(i) rate limit on a flapping facility (storage): the first ENTER emits,
// a re-entry inside kSensorFaultRepeatMs is suppressed, and after the window
// a fresh ENTER emits again.
void test_events_rate_limit_then_reedge_after_window(void) {
    FakeEventSink sink;
    FakeStorageHealth storage;
    storage.present = true;
    storage.health = reliability_test::corruptStorage(1000);
    (void)reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    f26::attachStorageHealth(&storage);

    f26::runTick(1000);  // ENTER -> event #1
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);

    f26::runTick(2000);  // still held -> nothing
    storage.health = reliability_test::healthyStorage(3000);
    f26::runTick(3000);  // EXIT (no event for exits)
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);

    storage.health = reliability_test::corruptStorage(4000);
    f26::runTick(4000);  // re-ENTER inside 60 s -> rate limited
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);

    storage.health = reliability_test::healthyStorage(5000);
    f26::runTick(5000);  // exit again
    storage.health = reliability_test::corruptStorage(70000);
    f26::runTick(70000);  // re-ENTER past kSensorFaultRepeatMs -> event #2
    TEST_ASSERT_EQUAL_UINT32(2, sink.count);
    assertFaultEvent(
        sink.events[1], bw::FaultCode::StorageDegraded,
        static_cast<uint8_t>(bw::reliability::facilityBit(bw::DegradedFacility::StorageDegraded)),
        70000);
}

// D13(ii): an anomalous boot (row 8: TWDT reset) yields EXACTLY ONE
// SensorFault for the whole boot; a boot-level observation carries no
// facility bit (payload[1] = 0).
void test_events_anomalous_boot_emits_once(void) {
    FakeEventSink sink;
    (void)reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    f26::noteBoot(bw::reliability::ResetReason::TaskWatchdog, 0);

    f26::runTick(1);
    f26::runTick(2);
    f26::runTick(3);
    TEST_ASSERT_EQUAL_UINT32(1, sink.count);
    assertFaultEvent(sink.events[0], bw::FaultCode::WatchdogReset, 0, 1);
}

// An ordinary boot (power-on/software) must stay silent forever — no event
// noise for normal resets (D13(ii)).
void test_events_ordinary_boot_emits_nothing(void) {
    FakeEventSink sink;
    (void)reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    f26::noteBoot(bw::reliability::ResetReason::PowerOn, 0);

    f26::runTick(1);
    f26::runTick(60000);
    TEST_ASSERT_EQUAL_UINT32(0, sink.count);
    TEST_ASSERT_EQUAL_UINT32(0, f26::droppedEvents());
}

// Failure behavior: a refusing sink AND a missing sink both count as dropped
// events (counted, never ignored — the backlog 18 CR request wants a
// dedicated FaultCode; until then the counter is the observable).
void test_events_dropped_publishes_are_counted(void) {
    FakeEventSink sink;
    sink.fail = true;
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 500);
    f26::runTick(1000);
    TEST_ASSERT_EQUAL_UINT32(0, sink.count);  // refused, not recorded
    TEST_ASSERT_EQUAL_UINT32(1, f26::droppedEvents());

    // Missing sink (F17 not wired yet) is also a loss, still counted.
    faults = reliability_test::resetWiring();  // wire() detaches the sink
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 500);
    f26::runTick(1000);
    TEST_ASSERT_EQUAL_UINT32(1, f26::droppedEvents());
}

// CR-033(b): a dropped publish is not only counted — it is ALSO reported into
// the registry as FaultCode::EventPublishDrop (28), so the loss is visible to
// CR-030 consumers (ReliabilityState.fault_bits), not just droppedEvents().
// The bit lands on the NEXT tick's snapshot: within one runTick the state is
// published (step 6) before the facility events are attempted (step 7).
// Reverting the report leaves bit 28 unset and fails here.
void test_events_dropped_publish_reports_event_publish_drop(void) {
    FakeEventSink sink;
    sink.fail = true;
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    f26::attachEventSink(&sink);
    faults->report(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error, 500);

    f26::runTick(1000);  // ENTER edge -> refused by the sink -> counted + reported
    TEST_ASSERT_EQUAL_UINT32(1, f26::droppedEvents());

    f26::runTick(2000);  // flag still held: no new edge, no second drop
    TEST_ASSERT_EQUAL_UINT32(1, f26::droppedEvents());

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(state));
    TEST_ASSERT_TRUE(
        (state.fault_bits & (1u << static_cast<uint8_t>(bw::FaultCode::EventPublishDrop))) != 0);
    // Its own symptom, not a stall: the heartbeat path stays clean.
    TEST_ASSERT_EQUAL_HEX32(0, state.fault_bits &
                                   (1u << static_cast<uint8_t>(bw::FaultCode::TaskStalled)));
}
