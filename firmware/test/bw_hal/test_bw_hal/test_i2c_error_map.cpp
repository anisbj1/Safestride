// Host tests for the pure Wire error mapping (F01 D6/D10): pins the
// Arduino-ESP32 endTransmission() contract (Wire.cpp documents 0..5) and the
// read-phase classification that keeps an unplugged device on the NotFound
// path instead of the bus-recovery path (i2c_bus.cpp policy).

#include <unity.h>

#include <bw/hal/i2c_error_map.h>

using bw::Err;
using bw::hal::errFromEndTransmission;
using bw::hal::errFromReadFailure;
using bw::hal::kWireAddrNack;
using bw::hal::kWireDataNack;
using bw::hal::kWireDataTooLong;
using bw::hal::kWireOtherError;
using bw::hal::kWireSuccess;
using bw::hal::kWireTimeout;

void test_errmap_success_is_ok(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Ok),
                          static_cast<int>(errFromEndTransmission(kWireSuccess)));
}

void test_errmap_addr_nack_is_not_found(void) {
    // Device-level fact: I2cBus stops its retry/recovery ladder on NotFound,
    // so an unplugged sensor must land here, never on Timeout/BusError.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::NotFound),
                          static_cast<int>(errFromEndTransmission(kWireAddrNack)));
}

void test_errmap_timeout_is_timeout(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Timeout),
                          static_cast<int>(errFromEndTransmission(kWireTimeout)));
}

void test_errmap_data_nack_too_long_and_other_are_bus_error(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromEndTransmission(kWireDataTooLong)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromEndTransmission(kWireDataNack)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromEndTransmission(kWireOtherError)));
}

void test_errmap_unknown_code_is_bus_error(void) {
    // A future core code must degrade to "transfer did not complete", not to
    // Ok and not to NotFound (which would wrongly disarm bus recovery).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromEndTransmission(6)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromEndTransmission(255)));
}

void test_read_failure_probe_ack_means_timeout(void) {
    // Device answers the probe but the read failed: the core hid the cause
    // (requestFrom returns 0 for NACK and timeout alike) — the bounded,
    // retryable Timeout is reported, never Ok and never NotFound.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Timeout),
                          static_cast<int>(errFromReadFailure(kWireSuccess)));
}

void test_read_failure_probe_nack_means_not_found(void) {
    // Device stopped answering between write and read: absent, not a bus
    // fault — no 9-SCL recovery for an unplugged sensor.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::NotFound),
                          static_cast<int>(errFromReadFailure(kWireAddrNack)));
}

void test_read_failure_probe_timeout_and_other_map_through(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::Timeout),
                          static_cast<int>(errFromReadFailure(kWireTimeout)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Err::BusError),
                          static_cast<int>(errFromReadFailure(kWireDataNack)));
}
