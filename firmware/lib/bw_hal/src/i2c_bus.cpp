#include <bw/hal/i2c_bus.h>

namespace bw::hal {

bw::Result<void> I2cBus::write(uint8_t addr7, const uint8_t* tx, size_t tx_len) {
    return doTransfer(addr7, tx, tx_len, nullptr, 0);
}

bw::Result<void> I2cBus::writeRead(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                                   size_t rx_len) {
    return doTransfer(addr7, tx, tx_len, rx, rx_len);
}

bw::Err I2cBus::scan(uint8_t* found, size_t max_found, size_t* count_out) {
    if (found == nullptr || count_out == nullptr || max_found == 0) {
        return bw::Err::InvalidArg;
    }
    *count_out = 0;
    for (unsigned addr = kI2cScanAddrMin; addr <= kI2cScanAddrMax; ++addr) {
        const bw::Result<void> probe = write(static_cast<uint8_t>(addr), nullptr, 0);
        if (probe.err == bw::Err::NotFound) {
            continue;  // nobody home — the expected answer for ~110 addresses
        }
        if (!probe.ok()) {
            // Bus-level trouble (or contention): stop here. Continuing would
            // repeat the whole outage ladder once per address — the D7 problem
            // at 112x scale — and a contention Busy is just as fatal to a
            // diagnostic walk as a dead bus.
            return probe.err;
        }
        found[(*count_out)++] = static_cast<uint8_t>(addr);
        if (*count_out == max_found) {
            break;  // caller gave us room for this many; do not overflow `found`
        }
    }
    return bw::Err::Ok;
}

bw::Err I2cBus::attemptPhase(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                             size_t rx_len) {
    // Once an outage is declared (any latch set), fail fast: a single attempt
    // keeps a dead bus close to one transaction of blocking per call instead of
    // paying the full retry ladder on every sensor cycle (F26 owns back-off and
    // reinit scheduling; F01 just must not stall SensorTask — spec §5).
    const uint8_t attempts = (timeout_reported_ || stuck_reported_) ? 1 : kI2cTransferAttempts;
    bw::Err err = bw::Err::Ok;
    for (uint8_t i = 0; i < attempts; ++i) {
        err = backend_.transfer(addr7, tx, tx_len, rx, rx_len, kI2cTransactionTimeoutMs);
        // NACK = the addressed device did not answer. That is a device-level
        // fact (driver reports *Absent), not a bus fault: stop immediately, do
        // not retry and do not pulse the bus.
        if (err == bw::Err::Ok || err == bw::Err::NotFound) {
            return err;
        }
    }
    return err;
}

bw::Result<void> I2cBus::doTransfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                                    size_t rx_len) {
    if (!backend_.lock(kI2cLockTimeoutMs)) {
        // Contention, not a bus fault: no recovery, no fault report — F23 sees
        // the resulting task lateness instead.
        return bw::Result<void>{bw::Err::Busy};
    }

    bw::Err err = attemptPhase(addr7, tx, tx_len, rx, rx_len);

    const bool bus_level_trouble =
        (err == bw::Err::Timeout || err == bw::Err::BusError) && !stuck_reported_;
    if (bus_level_trouble) {
        // One I2cTimeout per outage (edge-triggered rate limit, F01 D7), then
        // escalate: clock out 9 SCL pulses and retry once if the bus looks idle.
        reportEdge(bw::FaultCode::I2cTimeout, bw::FaultSeverity::Warning);
        if (backend_.recover()) {
            err = attemptPhase(addr7, tx, tx_len, rx, rx_len);
        }
        if (err != bw::Err::Ok && err != bw::Err::NotFound) {
            // Recovery did not clear it: the whole bus (all three devices) is
            // unreachable — one I2cBusStuck per outage.
            reportEdge(bw::FaultCode::I2cBusStuck, bw::FaultSeverity::Error);
        }
    }

    backend_.unlock();

    if (err == bw::Err::Ok) {
        rearm();  // next outage reports again
    }
    return bw::Result<void>{err};
}

void I2cBus::reportEdge(bw::FaultCode code, bw::FaultSeverity severity) {
    bool already_reported =
        (code == bw::FaultCode::I2cTimeout) ? timeout_reported_ : stuck_reported_;
    if (code == bw::FaultCode::I2cTimeout) {
        timeout_reported_ = true;
    } else {
        stuck_reported_ = true;
    }
    if (already_reported || faults_ == nullptr) {
        return;
    }
    // Severity is F01's initial mapping (fault.h leaves policy to F26):
    // Warning = this transaction failed but the bus may work next time;
    // Error   = no device on the bus answers until recovery/reinit succeeds.
    const bw::TimeMs t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
    faults_->report(code, severity, t_ms);
}

void I2cBus::rearm() {
    timeout_reported_ = false;
    stuck_reported_ = false;
}

}  // namespace bw::hal
