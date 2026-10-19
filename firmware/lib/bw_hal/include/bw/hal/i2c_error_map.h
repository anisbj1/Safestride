#pragma once

// bw/hal/i2c_error_map.h — Arduino-ESP32 Wire error codes -> bw::Err (F01 D6).
//
// Pure and host-tested (F01 D10 classification): the return codes of
// TwoWire::endTransmission() are documented in the core (Wire.cpp: "0 success,
// 1 data too long, 2 NACK on address, 3 NACK on data, 4 other error,
// 5 timeout") and are part of this HAL's contract with the platform, so they
// are pinned by host tests instead of living as a switch inside a platform
// file that only the bench could regress-test.

#include <cstdint>

#include <bw/result.h>

namespace bw::hal {

// [HW] Return codes of TwoWire::endTransmission() (Arduino-ESP32 core 2.x,
// Wire.cpp — verified against the framework source in the F01 part-2 session).
constexpr uint8_t kWireSuccess = 0;
constexpr uint8_t kWireDataTooLong = 1;  // tx exceeds the Wire buffer
constexpr uint8_t kWireAddrNack = 2;     // no device answered the address
constexpr uint8_t kWireDataNack = 3;     // device NACKed mid-byte
constexpr uint8_t kWireOtherError = 4;   // not initialized / bus in slave mode
constexpr uint8_t kWireTimeout = 5;      // transfer deadline exceeded

// Map one endTransmission() result to Err: Ok, NotFound (address NACK = the
// device is absent — I2cBus must NOT treat that as a bus fault), Timeout, and
// everything else to BusError (data NACK / buffer / driver-internal errors are
// all "the transfer did not complete").
bw::Err errFromEndTransmission(uint8_t wire_code);

// Classify a failed read phase. TwoWire::requestFrom() collapses every error
// into "0 bytes returned" (Wire.cpp: i2cWriteReadNonStop sets readCount = 0 on
// NACK *and* on timeout), so the backend re-asks the bus with an address-only
// probe — `probe_code` is that probe's endTransmission() result:
//   probe 2 (NACK)  -> NotFound: the device stopped answering (absent).
//   probe 0 (ACK)   -> Timeout: the device answers but the read itself failed;
//                      the core cannot say NACK-vs-timeout, so the bounded
//                      timeout is the honest, non-misleading choice (it keeps
//                      I2cBus' outage policy — report once, recover — intact).
//   probe 5 / other -> the mapped error (bus-level trouble).
bw::Err errFromReadFailure(uint8_t probe_code);

}  // namespace bw::hal
