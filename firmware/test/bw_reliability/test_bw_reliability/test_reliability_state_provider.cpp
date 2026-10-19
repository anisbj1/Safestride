// bw_reliability — CR-030 seam tests (spec §16.8 item 6): the
// IReliabilityStateProvider contract (false before the first tick, fields
// propagate) and the superset property F23-full relies on (the registry ⊇
// F23's own locally accumulated bits — bw/fault.h gap, CR-030).
//
// wire_reliability.cpp is included by test_wire_reliability_compile.cpp
// only (the one-TU rule); this TU links against f26:: through
// wire_reliability.h.

#include "../../../src/app/wiring/wire_reliability.h"

#include <unity.h>

#include <bw/reliability/degradation_policy.h>
#include <bw/reliability/reliability_config.h>

#include "reliability_test_support.h"

namespace {

constexpr uint32_t codeBit(const bw::FaultCode code) {
    return 1u << static_cast<uint8_t>(code);
}

}  // namespace

// The provider honours the "false = no snapshot yet" rule exactly like the
// accessor — consumers must treat that as absent, never as healthy (00 §1).
void test_state_provider_false_before_first_tick(void) {
    (void)reliability_test::resetWiring();
    bw::ReliabilityState state{};
    TEST_ASSERT_FALSE(f26::stateProvider().latest(state));
    TEST_ASSERT_FALSE(f26::latestReliabilityState(state));
}

// One tick publishes every CR-030 field; the accessor and the provider view
// return the SAME snapshot — one mailbox, two views (CR-030).
void test_state_provider_propagates_all_fields(void) {
    bw::IFaultReporter* faults = reliability_test::resetWiring();
    faults->report(bw::FaultCode::BleDown, bw::FaultSeverity::Warning, 500);
    f26::runTick(1000);

    bw::ReliabilityState via_accessor{};
    bw::ReliabilityState via_provider{};
    TEST_ASSERT_TRUE(f26::latestReliabilityState(via_accessor));
    TEST_ASSERT_TRUE(f26::stateProvider().latest(via_provider));
    TEST_ASSERT_EQUAL_UINT64(via_accessor.t_ms, via_provider.t_ms);
    TEST_ASSERT_EQUAL_HEX32(via_accessor.fault_bits, via_provider.fault_bits);
    TEST_ASSERT_EQUAL_HEX32(via_accessor.degraded, via_provider.degraded);
    TEST_ASSERT_EQUAL(via_accessor.safe_mode, via_provider.safe_mode);

    TEST_ASSERT_EQUAL_UINT64(1000, via_provider.t_ms);
    TEST_ASSERT_TRUE((via_provider.fault_bits & codeBit(bw::FaultCode::BleDown)) != 0);
    TEST_ASSERT_EQUAL_HEX32(0, via_provider.degraded);  // code alone != capability down
    TEST_ASSERT_FALSE(via_provider.safe_mode);
}

// The superset property: codes F23 reports through ctx.faults (its local
// accumulation) are all present in F26's published word, plus codes F26
// itself latched from the degradation table (row 3). This is the contract
// F23-full fills HealthSnapshot.fault_bits with (CR-030 semantics).
void test_state_provider_is_superset_of_f23_local_bits(void) {
    bw::IFaultReporter* faults = reliability_test::resetWiring();

    // F23's side: it reports its own symptoms and remembers them locally
    // (its pre-registry fallback behaviour — F23 spec §15.8).
    uint32_t f23_local_bits = 0;
    faults->report(bw::FaultCode::ImuStuckSensor, bw::FaultSeverity::Error, 100);
    f23_local_bits |= codeBit(bw::FaultCode::ImuStuckSensor);
    faults->report(bw::FaultCode::LowHeap, bw::FaultSeverity::Warning, 200);
    f23_local_bits |= codeBit(bw::FaultCode::LowHeap);

    // F26's side: the table latches its own row code through the wiring.
    reliability_test::FakeStorageHealth storage;
    storage.present = true;
    storage.health = reliability_test::corruptStorage(1000);
    f26::attachStorageHealth(&storage);
    f26::runTick(1000);

    bw::ReliabilityState state{};
    TEST_ASSERT_TRUE(f26::stateProvider().latest(state));
    TEST_ASSERT_TRUE((state.fault_bits & f23_local_bits) == f23_local_bits);  // superset
    TEST_ASSERT_TRUE((state.fault_bits & codeBit(bw::FaultCode::StorageDegraded)) != 0);
    TEST_ASSERT_TRUE((state.degraded &
                      bw::reliability::facilityBit(bw::DegradedFacility::StorageDegraded)) != 0);
}
