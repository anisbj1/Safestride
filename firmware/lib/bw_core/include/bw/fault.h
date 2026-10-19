#pragma once

// bw/fault.h — docs/02_INTERFACES.md §5 (IFaultReporter) and §7 (FaultCode /
// FaultSeverity, defined by F00 — full definitions in F00's spec §16).
//
// Append-only from here on (02 §6.6): every value below is pinned by
// test_fault_codes.cpp, so renumbering fails the build. New symptoms arrive via CR.
//
// F00 deliberately does NOT assign a severity per code: that mapping is policy and
// belongs to F26's degradation table (F26 §6 D2). F00 supplies the vocabulary only.

#include <cstdint>

#include <bw/types.h>

namespace bw {

enum class FaultSeverity : uint8_t {
    Info,      // record for diagnostics; no capability change (e.g. BatterySenseUnavailable)
    Warning,   // still functioning, F23 downgrades quality (e.g. PpgPoorContact, LowBattery)
    Error,     // source unusable; F26 degrades to a reduced pipeline (e.g. ImuReadFailed)
    Critical,  // system risk: safe mode / watchdog path implicated (e.g. BootLoop)
};

// 29 values, each derived from a symptom a current spec names today (F00 §16;
// #26 = CR-031, #27 = CR-032, #28/#29 = CR-033); nothing speculated, so no value
// is dead weight.
enum class FaultCode : uint8_t {
    None = 0,  // "no fault" — avoids callers inventing a 0-means-what convention
    // ── bus / HAL ────────────────────────────────────────────────────────────────
    I2cTimeout,               // F01 D1: per-transaction timeout
    I2cBusStuck,              // F01 D1: stuck-bus recovery (9 SCL pulses) could not clear it
    ImuAbsent,                // F01 §10: absent at boot; A-05 WHO_AM_I mismatch
    ImuReadFailed,            // F01 §10: consecutive read failures -> SensorStatus::Failed
    PpgAbsent,                // F01 §10: absent at boot; part ID mismatch
    PpgReadFailed,            // F01 §10: consecutive read failures -> SensorStatus::Failed
    PpgFifoOverrun,           // F01 §9: FIFO not drained within its poll interval
    DisplayFailed,            // F01 §10: display failure must never block sensing
    BatterySenseUnavailable,  // F01/A-04: no ADC divider -> battery reported "unknown"
    ButtonStuck,              // F11: pressed > 10 s -> ignore the input and report
    // ── data quality ─────────────────────────────────────────────────────────────
    ImuStuckSensor,   // F23 D2: run-length of identical values > kStuckSamples
    PpgFlatline,      // F23 D2: flat-line check
    PpgSaturated,     // F23 D2: PpgSample::saturated held across a window
    PpgPoorContact,   // F23 §3: poor contact / low perfusion
    TaskOverrun,      // F23 §8: lateness > kMaxTaskLatenessMs -> processing overload
    LowHeap,          // F23 §3: heap statistics below headroom
    LowBattery,       // F23 §8: battery < kLowBatteryPercent
    CriticalBattery,  // F23 §8: battery < kCriticalBatteryPercent
    // ── reliability ──────────────────────────────────────────────────────────────
    TaskStalled,      // F26 D1: heartbeat missing inside its deadline window
    WatchdogReset,    // F26 §3: reset reason capture shows a watchdog reset
    Brownout,         // F26 §6: brownout protection tripped
    BootLoop,         // F26 §6: >= kMaxRebootsInWindow inside kRebootWindowMin
    StorageDegraded,  // F26 §3: degraded flag "storage degraded"
    BleDown,          // F26 §3: degraded flag "BLE down"
    // ── personalization ─────────────────────────────────────────────────────────
    BaselineCorrupt,  // F13 §10 / CR-031: stored baseline profile corrupt or
                      // version-incompatible -> discarded, learning restarted
    // ── system overload (F23 M5, CR-032) ───────────────────────────────────────
    LowStack,  // F23 M5: TaskHealth.stack_free_min_bytes < kMinTaskStackFreeBytes
    // ── composition wiring (F26 backlogs 16/18, CR-033) ────────────────────────
    TaskCreateFailed,  // wire() xTaskCreate* != pdPASS: a task never started this boot
    EventPublishDrop,  // event sink missing/rejected a published event (counted at the site)
};

// Write-only from the reporter's side: features call report(), F26's registry
// aggregates. (Observed gap, D10: no read path back to F23's HealthSnapshot.fault_bits
// exists in 02 — that CR belongs to F23/F26, not F00.)
class IFaultReporter {
public:
    virtual void report(FaultCode, FaultSeverity, TimeMs) = 0;
};

}  // namespace bw
