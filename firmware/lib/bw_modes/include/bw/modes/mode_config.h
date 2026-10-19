#pragma once

// bw/modes/mode_config.h — F24 spec §8: every tunable the ModeManager reads,
// gathered into one injected struct (spec §6 D5). Today the defaults below are
// compile-time values; when a production ISettings table exists, a later CR
// appends SettingKeys and the wiring fills a ModeConfig from them — bw_modes
// code does not change.

#include <cstddef>
#include <cstdint>

namespace bw::modes {

// [EXPERIMENTAL] (was [USER] in the original §8; retagged by spec §6 D5 — no
// production ISettings exists yet, so [USER] would promise a runtime path that
// cannot exist. Becomes [USER] when a SettingKey CR lands.)
// Why 22: typical adult sleep window start; tune with recorded data (F28/F29).
constexpr uint8_t kNightStartHour = 22;
// [EXPERIMENTAL] Why 7: typical wake time; the pair defines the night window
// used by the D4 crossing rule.
constexpr uint8_t kNightEndHour = 7;

// [EXPERIMENTAL] LowPower enter threshold (battery percent). Initial guess;
// paired with the exit value below to form the D6 hysteresis band (15..25 %).
constexpr uint8_t kLowPowerEnterPercent = 15;
// [EXPERIMENTAL] LowPower exit threshold. Why 25: 10 percentage points above
// the enter value so charge-curve noise inside the band cannot flap the
// profile (spec §6 D6).
constexpr uint8_t kLowPowerExitPercent = 25;

// [DERIVED] Charging means mains power is available (D6 reasoning: the
// low-charge policy exists to conserve a draining battery), so charging
// overrides a low percentage and the profile returns to Normal.
constexpr bool kLowPowerChargingOverrides = true;

// [DERIVED] ServicePump cadence for f24::modePumpOnce() — integrator G1
// ruling 2 (2026-10-18): F24 rides the EXISTING ServicePump at its 2 s
// cadence (runtime.md §3.2; F20 precedent, no feature creates its own task);
// the plan's 1000 ms and the dedicated-task fallback were both rejected.
// Why 2 s is fine: the D4 crossing rule is edge-triggered, so a 2 s tick
// cannot MISS a boundary — it only delays it by ≤ 2 s, invisible at user
// scale. The composition root paces the call; modePumpOnce() itself does no
// pacing.
constexpr uint32_t kModePumpPeriodMs = 2000;

// [DERIVED] Bounded work per pump tick (spec §6 D9): at most this many queued
// commands are drained per 2 s tick; leftovers wait for the next tick, so a
// command burst cannot monopolize the ServicePump.
constexpr uint8_t kMaxCommandsPerPump = 4;

// [EXPERIMENTAL] F24-owned command-queue depth (spec §7.2 fan-out seam): a
// worst-case burst is a button gesture plus a BLE command arriving together;
// 8 slots of static storage is generous headroom, and a full queue refuses the
// push (CR-004 Full semantics) rather than blocking any producer.
constexpr size_t kModeCommandQueueDepth = 8;

struct ModeConfig {
    uint8_t night_start_hour = kNightStartHour;
    uint8_t night_end_hour = kNightEndHour;
    uint8_t low_power_enter_pct = kLowPowerEnterPercent;
    uint8_t low_power_exit_pct = kLowPowerExitPercent;
};

}  // namespace bw::modes
