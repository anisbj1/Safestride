#pragma once

// bw_modes host-suite helpers (private to test/bw_modes — allowed by
// folder"). Builders only; no assertions, no state.

#include <bw/commands.h>
#include <bw/hardware.h>
#include <bw/modes/mode_config.h>
#include <bw/modes/mode_manager.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw_modes_test {

inline bw::Command makeCommand(bw::CommandType type, uint32_t arg0 = 0) {
    bw::Command c{};
    c.type = type;
    c.t_ms = 0;
    c.source = 0;  // Button | Ble | Internal — the manager does not care
    c.arg0 = arg0;
    c.arg1 = 0;
    return c;
}

// Honest "no battery data" (spec §10): valid=false dominates everything.
inline bw::BatteryReading noBattery() {
    bw::BatteryReading b{};
    b.valid = false;
    return b;
}

inline bw::BatteryReading batteryAt(float percent) {
    bw::BatteryReading b{};
    b.valid = true;
    b.voltage_v = 3.7f;
    b.percent_estimate = percent;
    b.charging_known = false;
    b.charging = false;
    return b;
}

inline bw::BatteryReading chargingBattery(float percent) {
    bw::BatteryReading b{};
    b.valid = true;
    b.voltage_v = 4.1f;
    b.percent_estimate = percent;
    b.charging_known = true;
    b.charging = true;
    return b;
}

// Wall-clock seconds for a local hour/min/sec (unix epoch day — only the
// hour-of-day matters to the D4 crossing rule).
inline bw::UnixSeconds unixAt(uint8_t hour, uint8_t minute = 0, uint8_t second = 0) {
    return static_cast<bw::UnixSeconds>(hour * 3600u + minute * 60u + second);
}

// A settle tick: no command, unknown wall time unless asked, unknown battery.
inline bw::modes::ModeInput settleAt(bw::UnixSeconds unix_s = 0) {
    bw::modes::ModeInput in{};
    in.t_ms = 0;
    in.unix_s = unix_s;
    in.has_command = false;
    in.battery = noBattery();
    return in;
}

inline bw::modes::ModeInput settleWithBattery(bw::UnixSeconds unix_s,
                                              const bw::BatteryReading& battery) {
    bw::modes::ModeInput in = settleAt(unix_s);
    in.battery = battery;
    return in;
}

// A command tick: one explicit command, otherwise like settleAt().
inline bw::modes::ModeInput commandAt(const bw::Command& cmd, bw::UnixSeconds unix_s = 0) {
    bw::modes::ModeInput in = settleAt(unix_s);
    in.has_command = true;
    in.command = cmd;
    return in;
}

inline bw::modes::ModeInput commandWithBattery(const bw::Command& cmd,
                                               const bw::BatteryReading& battery) {
    bw::modes::ModeInput in = commandAt(cmd);
    in.battery = battery;
    return in;
}

}  // namespace bw_modes_test
