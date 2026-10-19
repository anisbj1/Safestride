// I2cBus policy tests (F01 §9: "I2cBus logic with a fake Wire — timeout, retry
// counting, recovery trigger"). The fake stands in for Wire behind
// II2cBackend (F01 D6); every assertion is about policy, not mechanism.

#include <unity.h>

#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_fault_reporter.h>

#include <bw/hal/i2c_bus.h>

#include "fake_i2c_backend.h"

using bw::Err;
using bw::FaultCode;
using bw::FaultSeverity;
using bw::hal::I2cBus;
using bw::hal::test::FakeI2cBackend;
using bw::testkit::FakeClock;
using bw::testkit::RecordingFaultReporter;

namespace {

constexpr uint8_t kAddrImu = 0x68;  // [HW] MPU6050 7-bit address (00 §3)

void assertOk(const bw::Result<void>& r) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(r.err));
}

void assertErr(const bw::Result<void>& r, Err expected) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(r.err));
}

// Same, for APIs that return a bare Err (I2cBus::scan).
void assertErrCode(bw::Err actual, Err expected) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

}  // namespace

void test_i2cbus_write_success_locks_and_unlocks_once(void) {
    FakeI2cBackend backend;
    I2cBus bus(backend, nullptr, nullptr);
    const uint8_t tx[] = {0x3B, 0x00};

    assertOk(bus.write(kAddrImu, tx, sizeof(tx)));

    TEST_ASSERT_EQUAL_UINT(1, backend.lockCount());
    TEST_ASSERT_EQUAL_UINT(1, backend.unlockCount());
    TEST_ASSERT_EQUAL_UINT(1, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT8(kAddrImu, backend.callAt(0).addr7);
    TEST_ASSERT_EQUAL_UINT(sizeof(tx), backend.callAt(0).tx_len);
    TEST_ASSERT_EQUAL_UINT(0, backend.callAt(0).rx_len);
    // Every blocking call carries the documented timeout (00 §7.2).
    TEST_ASSERT_EQUAL_UINT64(bw::hal::kI2cLockTimeoutMs, backend.lastLockTimeoutMs());
    TEST_ASSERT_EQUAL_UINT64(bw::hal::kI2cTransactionTimeoutMs, backend.lastTransferTimeoutMs());
}

void test_i2cbus_write_read_passes_rx_buffer(void) {
    FakeI2cBackend backend;
    I2cBus bus(backend, nullptr, nullptr);
    const uint8_t tx[] = {0x75};  // WHO_AM_I register
    uint8_t rx[1] = {0};

    assertOk(bus.writeRead(kAddrImu, tx, sizeof(tx), rx, sizeof(rx)));

    TEST_ASSERT_EQUAL_UINT(1, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT(1, backend.callAt(0).tx_len);
    TEST_ASSERT_EQUAL_UINT(1, backend.callAt(0).rx_len);
    TEST_ASSERT_EQUAL_UINT(1, backend.unlockCount());
}

void test_i2cbus_nack_stops_immediately_no_recovery_no_fault(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::NotFound);  // device absent -> NACK
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::NotFound);

    // A NACK is a device fact, not a bus fault: no retry, no recovery pulse.
    TEST_ASSERT_EQUAL_UINT(1, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT(0, backend.recoverCount());
    TEST_ASSERT_EQUAL_UINT(0, faults.count());
    TEST_ASSERT_EQUAL_UINT(1, backend.unlockCount());  // always released
}

void test_i2cbus_transient_timeout_retries_within_attempts(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::Timeout);  // first attempt glitches, second is fine
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertOk(bus.write(kAddrImu, tx, sizeof(tx)));

    TEST_ASSERT_EQUAL_UINT(2, backend.transferCount());  // kI2cTransferAttempts
    TEST_ASSERT_EQUAL_UINT(0, backend.recoverCount());   // no escalation
    TEST_ASSERT_EQUAL_UINT(0, faults.count());           // no report for a blip
}

void test_i2cbus_outage_reports_timeout_recovers_and_succeeds(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::Timeout);
    backend.pushOutcome(Err::Timeout);  // phase 1 exhausted -> escalate
    // recover() returns true (default), retry (phase 2) succeeds.
    RecordingFaultReporter<4> faults;
    FakeClock clock;
    clock.setMs(1234);
    I2cBus bus(backend, &clock, &faults);
    const uint8_t tx[] = {0x00};

    assertOk(bus.write(kAddrImu, tx, sizeof(tx)));

    TEST_ASSERT_EQUAL_UINT(3, backend.transferCount());  // 2 attempts + 1 retry
    TEST_ASSERT_EQUAL_UINT(1, backend.recoverCount());   // exactly one escalation
    // One report, on the edge, stamped from the clock (D7).
    TEST_ASSERT_EQUAL_UINT(1, faults.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultCode::I2cTimeout),
                          static_cast<int>(faults.at(0).code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultSeverity::Warning),
                          static_cast<int>(faults.at(0).severity));
    TEST_ASSERT_EQUAL_UINT64(1234, faults.at(0).t_ms);
}

void test_i2cbus_failed_recovery_reports_stuck(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::Timeout);
    backend.pushOutcome(Err::Timeout);
    backend.setRecoverResult(false);   // 9 SCL pulses did not clear the bus
    backend.setDefault(Err::Timeout);  // retry phase skipped anyway
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Timeout);

    TEST_ASSERT_EQUAL_UINT(2, backend.transferCount());  // retry skipped: recover() false
    TEST_ASSERT_EQUAL_UINT(1, backend.recoverCount());
    TEST_ASSERT_EQUAL_UINT(2, faults.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultCode::I2cTimeout),
                          static_cast<int>(faults.at(0).code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultCode::I2cBusStuck),
                          static_cast<int>(faults.at(1).code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultSeverity::Error),
                          static_cast<int>(faults.at(1).severity));
    TEST_ASSERT_EQUAL_UINT(1, backend.unlockCount());
}

void test_i2cbus_declared_outage_fails_fast_single_attempt(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::Timeout);
    backend.pushOutcome(Err::Timeout);
    backend.setRecoverResult(false);
    backend.setDefault(Err::Timeout);  // the bus stays dead
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Timeout);  // declares the outage
    const size_t calls_after_outage = backend.transferCount();
    const size_t faults_after_outage = faults.count();

    // Second sensor cycle while the outage is declared: one attempt, no
    // recovery attempt, no repeated reports (rate limit + fail fast, D7).
    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Timeout);
    TEST_ASSERT_EQUAL_UINT(calls_after_outage + 1, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT(1, backend.recoverCount());
    TEST_ASSERT_EQUAL_UINT(faults_after_outage, faults.count());
}

void test_i2cbus_success_rearms_so_next_outage_reports_again(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::Timeout);
    backend.pushOutcome(Err::Timeout);
    backend.setRecoverResult(false);
    backend.setDefault(Err::Timeout);
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Timeout);
    TEST_ASSERT_EQUAL_UINT(2, faults.count());  // I2cTimeout + I2cBusStuck

    backend.setRecoverResult(true);
    backend.setDefault(Err::Ok);  // wiring fixed: bus works again
    assertOk(bus.write(kAddrImu, tx, sizeof(tx)));
    TEST_ASSERT_EQUAL_UINT(2, faults.count());

    // A new outage after a success must report again (latches re-armed).
    backend.setRecoverResult(false);
    backend.setDefault(Err::Timeout);
    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Timeout);
    TEST_ASSERT_EQUAL_UINT(4, faults.count());
    TEST_ASSERT_EQUAL_UINT(2, faults.countOf(FaultCode::I2cBusStuck));
}

void test_i2cbus_lock_timeout_returns_busy_without_touching_the_bus(void) {
    FakeI2cBackend backend;
    backend.setLockResult(false);
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    const uint8_t tx[] = {0x00};

    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::Busy);

    TEST_ASSERT_EQUAL_UINT(0, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT(0, backend.unlockCount());  // no unlock without lock
    TEST_ASSERT_EQUAL_UINT(0, faults.count());         // contention is not a fault
}

void test_i2cbus_null_clock_and_reporter_still_returns_errors(void) {
    FakeI2cBackend backend;
    backend.setDefault(Err::BusError);
    I2cBus bus(backend, nullptr, nullptr);  // F00 §13: clock may be unassigned
    const uint8_t tx[] = {0x00};

    // Must not crash or hang; the caller still learns the truth.
    assertErr(bus.write(kAddrImu, tx, sizeof(tx)), Err::BusError);
    TEST_ASSERT_EQUAL_UINT(1, backend.unlockCount());
}

void test_i2cbus_scan_collects_acknowledging_addresses(void) {
    FakeI2cBackend backend;
    backend.pushOutcome(Err::NotFound);  // 0x08 absent, every later addr answers
    I2cBus bus(backend, nullptr, nullptr);
    uint8_t found[4] = {};
    size_t n = 99;  // must be overwritten, not appended to

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok), static_cast<int>(bus.scan(found, 4, &n)));

    TEST_ASSERT_EQUAL_UINT(4, n);             // capacity stops the walk
    TEST_ASSERT_EQUAL_UINT8(0x09, found[0]);  // the NACKed address is not listed
    TEST_ASSERT_EQUAL_UINT8(0x0A, found[1]);
    TEST_ASSERT_EQUAL_UINT8(0x0B, found[2]);
    TEST_ASSERT_EQUAL_UINT8(0x0C, found[3]);
    TEST_ASSERT_EQUAL_UINT(5, backend.transferCount());  // 1 NACK + 4 stored
    // Every probe is an address-only zero-length write (D13's ACK test): the
    // walk must not put bytes on the wire in front of a device.
    TEST_ASSERT_EQUAL_UINT(0, backend.callAt(0).tx_len);
    TEST_ASSERT_EQUAL_UINT(0, backend.callAt(0).rx_len);
    TEST_ASSERT_EQUAL_UINT8(0x08, backend.callAt(0).addr7);
    // A NACK is absence, not bus trouble: no recovery pulse, no fault report.
    TEST_ASSERT_EQUAL_UINT(0, backend.recoverCount());
    TEST_ASSERT_EQUAL_UINT(backend.transferCount(), backend.unlockCount());
}

void test_i2cbus_scan_aborts_on_bus_error_after_one_outage(void) {
    FakeI2cBackend backend;
    backend.setDefault(Err::Timeout);  // the bus never answers
    RecordingFaultReporter<4> faults;
    I2cBus bus(backend, nullptr, &faults);
    uint8_t found[8] = {};
    size_t n = 7;  // reset even on the error path

    assertErrCode(bus.scan(found, 8, &n), Err::Timeout);
    TEST_ASSERT_EQUAL_UINT(0, n);

    // The whole point of aborting: ONE outage's worth of work (2 attempts +
    // 1 recovery retry), never 112 of them — the scan runs every 2 s in the
    // bench loop and must not turn a dead bus into a multi-second stall.
    TEST_ASSERT_EQUAL_UINT(3, backend.transferCount());
    TEST_ASSERT_EQUAL_UINT(1, backend.recoverCount());
    // D7 edge rule still holds: one I2cTimeout + one I2cBusStuck per outage.
    TEST_ASSERT_EQUAL_UINT(2, faults.count());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultCode::I2cTimeout),
                          static_cast<int>(faults.at(0).code));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FaultCode::I2cBusStuck),
                          static_cast<int>(faults.at(1).code));
}

void test_i2cbus_scan_rejects_missing_arguments(void) {
    FakeI2cBackend backend;
    I2cBus bus(backend, nullptr, nullptr);
    uint8_t found[4] = {};
    size_t n = 0;

    assertErrCode(bus.scan(nullptr, 4, &n), Err::InvalidArg);
    assertErrCode(bus.scan(found, 0, &n), Err::InvalidArg);
    assertErrCode(bus.scan(found, 4, nullptr), Err::InvalidArg);

    TEST_ASSERT_EQUAL_UINT(0, backend.transferCount());  // nothing touched
}
