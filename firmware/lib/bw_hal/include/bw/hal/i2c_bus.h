#pragma once

// bw/hal/i2c_bus.h — the single arbiter of the shared I²C bus (F01 D1).
//
// Policy lives here and is pure: timeout budget, retry, stuck-bus recovery
// escalation, edge-triggered fault reporting (F01 D7). Mechanism (Wire, the
// FreeRTOS mutex, the 9-SCL-pulse recovery) sits behind II2cBackend (F01 D6).
// No other module touches I²C (00 §3): drivers call write()/writeRead() only.

#include <cstddef>
#include <cstdint>

#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/result.h>
#include <bw/types.h>

#include <bw/hal/i2c_backend.h>

namespace bw::hal {

// [EXPERIMENTAL] Per-transfer deadline. Why: longer than any normal transfer
// (a display flush chunk or a 14-byte register burst at 400 kHz is < 1 ms of
// wire time) yet short enough that one stuck transaction cannot blow the 20 ms
// SensorTask cycle (runtime.md §3.2). Tune with bench data (F28/F29).
constexpr bw::TimeMs kI2cTransactionTimeoutMs = 20;

// [EXPERIMENTAL] Mutex wait before giving up with Err::Busy. Why: the lock is
// held for at most one transaction (kI2cTransactionTimeoutMs), so waiting one
// transaction period guarantees entry under normal contention while keeping a
// 20 ms sensor cycle intact; longer waits would hide a starved task (F23 sees
// the lateness instead).
constexpr bw::TimeMs kI2cLockTimeoutMs = 20;

// [EXPERIMENTAL] Attempts per phase before escalating to bus recovery. Why 2:
// a single glitch must not escalate to clock-pulsing, which disturbs every
// device on the bus; two attempts still resolve a transient in < 2 timeouts.
constexpr uint8_t kI2cTransferAttempts = 2;

// [HW] 7-bit address window for the diagnostic scan: 0x08..0x77 is the whole
// 7-bit space minus the reserved control block 0x00-0x07 and the reserved
// 10-bit block 0x78-0x7F (I2C-bus specification, "Reserved 7-bit addresses";
// the same window the Arduino core's own bus scanner walks).
constexpr unsigned kI2cScanAddrMin = 0x08;
constexpr unsigned kI2cScanAddrMax = 0x77;

// Bus policy state machine (F01 D7):
//
//   transfer -> Ok .............................. re-arm fault latches, done
//   transfer -> Err::NotFound (NACK) ............ propagate; a missing device
//                                                 is not a bus fault (bus-level
//                                                 recovery would be wrong), the
//                                                 driver reports *Absent itself
//   transfer -> Timeout/BusError x attempts ..... report I2cTimeout (once per
//                                                 outage), pulse 9 SCL, retry
//   still failing after recovery ............... report I2cBusStuck (once per
//                                                 outage), return last Err
//
// Blocking budget — spec §5 "block longer than the documented I²C timeout" is
// read as per-transaction ([ASSUMED], status log 2026-10-01); every wait below
// carries a named timeout, nothing is ever unbounded (00 §7.2):
//   healthy call ........ lock (<= kI2cLockTimeoutMs, usually immediate)
//                         + 1 transfer (<= kI2cTransactionTimeoutMs)   <= 40 ms
//   transient blip ...... 2 attempts                              <= ~60 ms
//   first call of an outage ... 2 attempts + recovery + 1 retry    <= ~100 ms
//   declared outage ..... fail-fast: lock + 1 attempt               <= 40 ms
//
// "Once per outage" = edge-triggered rate limiting: the next Ok re-arms both
// latches. Simplest option that satisfies the rate-limit rule; F26 may add
// time-window suppression on top later (most scalable option, not needed now).
class I2cBus {
public:
    // `clock` and `faults` may be nullptr: the device IClock is an open
    // cross-feature question (F00 §13) and F26 owns the real reporter — until
    // they are wired, fault timestamps fall back to 0 (boot) and reports are
    // dropped, but the policy still returns correct errors.
    I2cBus(II2cBackend& backend, const bw::IClock* clock, bw::IFaultReporter* faults)
        : backend_(backend), clock_(clock), faults_(faults) {}

    // Pure write (rx absent). NACK from the device surfaces as Err::NotFound.
    bw::Result<void> write(uint8_t addr7, const uint8_t* tx, size_t tx_len);

    // Register write + repeated-start read (sensor access pattern).
    bw::Result<void> writeRead(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                               size_t rx_len);

    // Diagnostic bus walk (spec §9 bench checklist "I2C scan"): one
    // address-only probe per address — a zero-length write, the same ACK test
    // the core's scanner uses (see F01 D13) — through this class, so no other
    // module ever touches Wire (00 §3).
    //   Err::Ok            -> the window was walked (or `found` filled);
    //                         `*count_out` = number of addresses stored
    //   address that NACKs -> absent: skipped, never a result and never a fault
    //   any other Err      -> the bus itself failed; the walk aborts at once so
    //                         a dead bus costs at most ONE outage transaction
    //                         (<= ~100 ms, see the blocking budget above) rather
    //                         than 112. `*count_out` is set on every path.
    bw::Err scan(uint8_t* found, size_t max_found, size_t* count_out);

private:
    bw::Result<void> doTransfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                                size_t rx_len);
    bw::Err attemptPhase(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx,
                         size_t rx_len);
    void reportEdge(bw::FaultCode code, bw::FaultSeverity severity);
    void rearm();

    II2cBackend& backend_;
    const bw::IClock* clock_;
    bw::IFaultReporter* faults_;
    bool timeout_reported_ = false;  // one I2cTimeout per outage
    bool stuck_reported_ = false;    // one I2cBusStuck per outage
};

}  // namespace bw::hal
