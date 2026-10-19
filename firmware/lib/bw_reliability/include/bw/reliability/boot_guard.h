#pragma once

// bw/reliability/boot_guard.h — clock-free boot-loop protection and the safe
// mode decision (spec §16.1 D10, integrator-ruled at G1): streak =
// consecutive boots that did NOT survive kBootLoopBootLimitMs of uptime;
// safe mode = streak >= kMaxRebootsInWindow at boot (detection +
// publication only — whether the composition unwires F19/F20/F16/F28 in safe
// mode is the P4 integrator's call, G1 ruling 2).
//
// Pure: no RTC, no wall clock (runtime.md §4 — wall time may be 0), no
// storage. The wiring loads/stores the persisted record (crash_record.h).

#include <cstdint>

#include <bw/fault.h>
#include <bw/reliability/reliability_config.h>
#include <bw/types.h>

namespace bw::reliability {

// Captured once at boot from the platform (esp_reset_reason() at wiring,
// mapped in wire_reliability.cpp) and persisted in the crash record.
enum class ResetReason : uint8_t {
    Unknown = 0,   // no source or unmapped code — honest unknown, never "clean"
    PowerOn,       // external reset / power cycle
    Software,      // software restart
    Panic,         // software panic: no FaultCode exists (append-only, 02 §6.6),
                   // so it is recorded only — the streak still counts it
    IntWatchdog,   // interrupt watchdog
    TaskWatchdog,  // task watchdog (the D9 supervision path)
    Brownout,      // brownout detector
    DeepSleep,     // normal deep-sleep wake
};

class BootGuard {
public:
    // `streak_base` = the persisted streak from the crash record (0 when the
    // record is absent or corrupt). THIS boot counts too, so the effective
    // streak is base + 1: 2 fast boots => streak 2 => normal, the 3rd boot =>
    // streak 3 => safe mode (§16.8 test 3).
    void onBoot(uint8_t streak_base, ResetReason reason);

    // Returns true EXACTLY ONCE: the first call at/after
    // kBootLoopBootLimitMs of uptime while the streak is non-zero. The caller
    // then persists the cleared streak — D10's exit: the NEXT boot is
    // full-featured (no live unwiring of safe mode).
    bool tickSurvived(TimeMs now_ms, TimeMs boot_ms);

    uint8_t streak() const { return streak_; }
    bool safeMode() const { return safe_mode_; }
    ResetReason reason() const { return reason_; }

    // Row 8: TWDT/INT-WDT => WatchdogReset, brownout => Brownout, everything
    // else => None (recorded in the crash record, no code invented).
    static FaultCode resetFaultCode(ResetReason reason);

private:
    uint8_t streak_ = 0;
    bool safe_mode_ = false;
    ResetReason reason_ = ResetReason::Unknown;
};

}  // namespace bw::reliability
