#pragma once

// bw/hal/i2c_backend.h — the narrow seam between I2cBus policy (pure, host-tested)
// and the platform mechanism (Wire + FreeRTOS mutex + SCL bit-bang on ESP32).
// F01 D6: everything that needs Arduino/FreeRTOS lives behind this interface so
// i2c_bus.cpp compiles and runs under `pio test -e native`.
//
// Only bw_hal ever implements this for the device; only I2cBus ever calls it.

#include <cstddef>
#include <cstdint>

#include <bw/result.h>
#include <bw/types.h>

namespace bw::hal {

class II2cBackend {
public:
    // Take/release the bus mutex. false = not acquired within timeout_ms
    // (contention, not a bus fault: the caller sees Err::Busy).
    virtual bool lock(bw::TimeMs timeout_ms) = 0;
    virtual void unlock() = 0;

    // One I²C transaction at 7-bit `addr7`: write tx_len bytes, then (repeated
    // start) read rx_len bytes when rx_len > 0; rx_len == 0 is a pure write.
    // timeout_ms bounds the transfer (Wire timeout on ESP32). Returns
    // Err::Ok / Err::Timeout / Err::BusError / Err::NotFound (NACK).
    virtual bw::Err transfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                             size_t rx_len, bw::TimeMs timeout_ms) = 0;

    // Stuck-bus recovery: clock out 9 SCL pulses with SDA released, then check
    // the bus. true = SDA high again (bus idle, safe to retry).
    // [HW] 9-pulse recovery is the standard I²C bus-recovery practice.
    virtual bool recover() = 0;
};

}  // namespace bw::hal
