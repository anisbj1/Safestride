#pragma once

// bw/hal/esp32_i2c_backend.h — the ESP32 mechanism behind II2cBackend (F01 D6).
// Platform-only file (F01 D5): whole file behind ARDUINO, so `pio test -e
// native` sees an empty translation unit. All *policy* (retry, outage latches,
// recovery escalation) lives in I2cBus and is host-tested; this file only moves
// bytes through Wire, guards the bus with a FreeRTOS mutex and bit-bangs the
// 9-SCL-pulse recovery.

#if defined(ARDUINO)

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <bw/hal/i2c_backend.h>

// The Arduino TwoWire instance is injected, not hard-coded: the backend header
// stays free of Wire.h (only the .cpp includes it), and a second bus
// controller (ESP32 has two) could get its own backend later.
class TwoWire;

namespace bw::hal {

// [HW] Fast-mode clock (F01 spec §8). Both MPU6050 and MAX30102 document
// 400 kHz fast-mode support; the SH1106 datasheet allows 400 kHz too, so one
// bus speed serves all three devices (00 §3: one shared bus).
constexpr uint32_t kI2cClockHz = 400000;

// [DERIVED] Wire TX/RX buffer size (Arduino-ESP32 core 2.x defaults to 128 B).
// Why 256: the largest single transfer is the MAX30102 FIFO drain —
// kPpgFifoDepth (32) × kPpgFifoBytesPerSample (6) = 192 B plus a 1-byte
// register pointer — so 128 B would truncate it; 256 B covers 193 B with
// margin and costs 128 B of heap once at Wire.begin() (startup, not a
// real-time path).
constexpr uint16_t kI2cWireBufferBytes = 256;

// [HW] Bus-recovery clock count (F01 D1, spec §9): the I²C standard recovery
// practice is 9 SCL pulses with SDA released, which shifts out any byte a
// stuck slave is holding mid-transfer.
constexpr uint8_t kI2cRecoveryPulseCount = 9;

// [DERIVED] Half-period of the bit-banged recovery clock (50 kHz). Why: above
// the 4 µs minimum SCL high time of any I²C device (slowest standard-mode
// part) and well under the chips' maximum ratings, so every device on the bus
// can follow it. Total pulse time = 9 × 2 × 10 µs = 180 µs — negligible inside
// the kI2cTransactionTimeoutMs budget.
constexpr uint32_t kI2cRecoveryHalfPeriodUs = 10;

// One instance, constructed and begin()-ed by wire_hal before any task exists.
// All Wire access happens between I2cBus' lock()/unlock() (i2c_bus.cpp), so
// this class needs no internal locking beyond the one mutex.
class Esp32I2cBackend final : public II2cBackend {
public:
    // `wire` is the Arduino bus instance (global `Wire` at wiring), pins come
    // from config/hw_pins.h.
    Esp32I2cBackend(TwoWire& wire, int sda_pin, int scl_pin)
        : wire_(wire), sda_pin_(sda_pin), scl_pin_(scl_pin) {}

    // Wire.begin + buffer/timeout + statically allocated mutex (no heap after
    // startup: StaticSemaphore_t is member storage, 00 §7.2). Returns false if
    // the mutex could not be created — wire_hal treats that as a boot fault and
    // leaves the bus unwired (lock() fails safe with Err::Busy).
    bool begin();

    bool lock(bw::TimeMs timeout_ms) override;
    void unlock() override;
    bw::Err transfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_len,
                     bw::TimeMs timeout_ms) override;
    bool recover() override;

    // Bench-only raw probe (F01 D16 anomaly triage, scaffolding — no feature
    // may call it): one register read straight through Wire, bypassing
    // transfer()'s deferred-write merging. stop_between=false repeats the
    // backend's own pattern (endTransmission(false) + requestFrom = one
    // repeated-start transaction); stop_between=true issues a STOP after the
    // register write and a fresh START for the read. Comparing the two splits
    // "Wire/core repeated-start path" from "bus/chip": if only the STOP
    // variant reads sane IDs, the fault is in the merged-read path, not the
    // hardware. No mutex: bench callers run on the loop task alone.
    bw::Err benchRawRead(uint8_t addr7, uint8_t reg, uint8_t* out, bool stop_between);

private:
    TwoWire& wire_;
    int sda_pin_;
    int scl_pin_;
    StaticSemaphore_t mutex_storage_ = {};
    SemaphoreHandle_t mutex_ = nullptr;  // null until begin() succeeds
    bool wire_up_ = false;
};

}  // namespace bw::hal

#endif  // ARDUINO
