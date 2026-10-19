#include <bw/hal/esp32_i2c_backend.h>

#if defined(ARDUINO)

#include <Arduino.h>
#include <Wire.h>

#include <bw/hal/i2c_bus.h>
#include <bw/hal/i2c_error_map.h>

namespace bw::hal {

namespace {
// [DERIVED] Floor for every wait we hand to FreeRTOS/core code. Why: a 0-tick
// wait would turn a "timeout" into an immediate failure (and pdMS_TO_TICKS
// rounds sub-tick values down to 0), so any positive budget becomes at least
// one tick; 0 stays 0 (try once, no wait — never unbounded, 00 §7.2).
TickType_t ticksFor(bw::TimeMs timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}
}  // namespace

bool Esp32I2cBackend::begin() {
    // Statically allocated mutex: member storage, no heap after startup
    // (00 §7.2). Created here, before any task exists (wire_hal call order).
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    if (mutex_ == nullptr) {
        return false;
    }
    // Buffer first: TwoWire allocates rx/tx buffers with the configured size
    // inside begin() (Wire.cpp), so setBufferSize must precede it (F01
    // part-2 plan; kI2cWireBufferBytes covers the 192 B PPG FIFO drain).
    wire_.setBufferSize(kI2cWireBufferBytes);
    wire_.setTimeOut(static_cast<uint16_t>(kI2cTransactionTimeoutMs));
    wire_up_ = wire_.begin(sda_pin_, scl_pin_, kI2cClockHz);
    return wire_up_;
}

bool Esp32I2cBackend::lock(bw::TimeMs timeout_ms) {
    if (mutex_ == nullptr) {
        // begin() was never called (or failed): fail safe — I2cBus turns this
        // into Err::Busy instead of letting Wire race across tasks.
        return false;
    }
    return xSemaphoreTake(mutex_, ticksFor(timeout_ms)) == pdTRUE;
}

void Esp32I2cBackend::unlock() {
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
}

bw::Err Esp32I2cBackend::transfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                                  size_t rx_len, bw::TimeMs timeout_ms) {
    if (mutex_ == nullptr || !wire_up_) {
        return bw::Err::NotReady;
    }
    if (rx_len > kI2cWireBufferBytes) {
        // The Wire buffer bounds one transfer (kI2cWireBufferBytes); a larger
        // request is a caller bug, not a bus condition — refuse it up front
        // instead of silently truncating (Err::Full would be wrong: nothing
        // is written).
        return bw::Err::InvalidArg;
    }
    // Per-transfer deadline: the core takes it in milliseconds as uint16_t.
    // Clamp the upper bound to the type; 0 keeps its contract meaning (bound
    // the transfer by not waiting — never unbounded, 00 §7.2).
    uint16_t timeout_ms16;
    if (timeout_ms == 0) {
        timeout_ms16 = 0;
    } else {
        timeout_ms16 = (timeout_ms >= 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(timeout_ms);
    }
    wire_.setTimeOut(timeout_ms16);

    // Write phase — also the whole transaction for a pure write or an
    // address-only probe (tx_len == rx_len == 0: no ID register exists on the
    // SH1106, so absence is detected with exactly this probe, spec §9).
    if (tx_len > 0 || rx_len == 0) {
        wire_.beginTransmission(addr7);
        if (tx_len > 0 && wire_.write(tx, tx_len) != tx_len) {
            // More bytes than the Wire buffer: end() to release the pending
            // transmission, then surface it as a completed-transfer failure.
            wire_.endTransmission(true);
            return bw::Err::BusError;
        }
        // sendStop = false defers the write: endTransmission() only marks the
        // buffer "nonStop" and returns 0 without touching the bus (Wire.cpp);
        // the bytes go out inside requestFrom as one repeated-start transaction
        // (sensor access pattern — register pointer + read, no STOP between).
        // Any real error then surfaces in the read phase below.
        const bool send_stop = (rx_len == 0);
        const uint8_t rc = wire_.endTransmission(send_stop);
        if (rc != kWireSuccess) {
            return errFromEndTransmission(rc);
        }
    }
    if (rx_len == 0) {
        return bw::Err::Ok;
    }

    // Read phase.
    if (rx == nullptr) {
        return bw::Err::InvalidArg;  // caller bug: no bus traffic for it
    }
    const size_t got = wire_.requestFrom(static_cast<uint16_t>(addr7), rx_len, true);
    if (got == rx_len) {
        // requestFrom() only fills Wire's internal buffer — drain it into the
        // caller. Without this loop every driver read returned the caller's
        // own uninitialized/stale bytes: per-boot frozen garbage samples,
        // boot-trail WHO_AM_I "0/FF", config echo of stack contents, while
        // writes, ACKs and the scan all worked (D16 anomaly, root cause —
        // found by the [RAW] probe: raw Wire read 0x68/0x55 on the same
        // bus where this path returned 0x00).
        for (size_t i = 0; i < rx_len; ++i) {
            rx[i] = static_cast<uint8_t>(wire_.read());
        }
        return bw::Err::Ok;
    }
    if (got == 0) {
        // requestFrom() returns 0 for a read-NACK and for a timeout alike
        // (Wire.cpp: readCount = 0 on any i2cWriteReadNonStop error), so ask
        // the bus again with an address-only probe to tell "device gone"
        // (NotFound) from "bus trouble" (Timeout). The probe runs only on the
        // failure path; it is a zero-length write, which the core explicitly
        // supports (esp32-hal-i2c.c: "short implementation does not support
        // zero size writes" — that is why the manual command link exists).
        wire_.beginTransmission(addr7);
        const uint8_t probe_rc = wire_.endTransmission(true);
        return errFromReadFailure(probe_rc);
    }
    // Partial read: the core never returns short reads without an error, so
    // treat it as an incomplete transfer.
    return bw::Err::BusError;
}

bw::Err Esp32I2cBackend::benchRawRead(uint8_t addr7, uint8_t reg, uint8_t* out, bool stop_between) {
    if (!wire_up_) {
        return bw::Err::NotReady;
    }
    // Write phase: the register pointer.
    wire_.beginTransmission(addr7);
    if (wire_.write(&reg, 1) != 1) {
        wire_.endTransmission(true);
        return bw::Err::BusError;
    }
    const uint8_t rc = wire_.endTransmission(stop_between);
    if (rc != kWireSuccess) {
        return errFromEndTransmission(rc);
    }
    // Read phase: with stop_between=false the bytes above still sit in the
    // buffer and go out here as a repeated start (transfer()'s pattern);
    // with stop_between=true they already went out behind a STOP, so this is
    // an independent read transaction.
    const size_t got =
        wire_.requestFrom(static_cast<uint16_t>(addr7), static_cast<size_t>(1), true);
    if (got == 0) {
        return bw::Err::Timeout;  // bench: count only, no second probe
    }
    if (got != 1) {
        return bw::Err::BusError;
    }
    *out = static_cast<uint8_t>(wire_.read());
    return bw::Err::Ok;
}

bool Esp32I2cBackend::recover() {
    if (mutex_ == nullptr || !wire_up_) {
        return false;
    }
    // Take the pins away from the I2C peripheral and drive them as GPIOs.
    // end() deinits the controller and frees the buffers; bufferSize is kept
    // (Wire.cpp freeWireBuffer), so the re-begin below re-allocates 256 B.
    wire_.end();
    pinMode(sda_pin_, INPUT_PULLUP);       // release SDA: a slave stuck mid-
                                           // byte must be able to shift out
    pinMode(scl_pin_, OUTPUT_OPEN_DRAIN);  // open-drain: HIGH = line released
    digitalWrite(scl_pin_, HIGH);
    // [HW] 9 SCL pulses with SDA released = the standard bus-recovery
    // practice (F01 D1): each clock advances a slave that is holding SDA low
    // until it finishes its byte and releases the line. The microsecond
    // busy-wait is bounded: 9 x 2 x kI2cRecoveryHalfPeriodUs = 180 us, far
    // below one transaction — not a blocking delay in a task (00 §7.2).
    for (uint8_t i = 0; i < kI2cRecoveryPulseCount; ++i) {
        digitalWrite(scl_pin_, LOW);
        delayMicroseconds(kI2cRecoveryHalfPeriodUs);
        digitalWrite(scl_pin_, HIGH);
        delayMicroseconds(kI2cRecoveryHalfPeriodUs);
    }
    const bool sda_released = (digitalRead(sda_pin_) == HIGH);

    // Hand the pins back to the Wire peripheral; setTimeOut persists (it is
    // re-applied per transfer anyway).
    wire_up_ = wire_.begin(sda_pin_, scl_pin_, kI2cClockHz);
    return sda_released && wire_up_;
}

}  // namespace bw::hal

#endif  // ARDUINO
