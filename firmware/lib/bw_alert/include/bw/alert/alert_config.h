#pragma once

// bw/alert/alert_config.h - F10 §8: every threshold, duration and mode profile
// the alert engine uses, as named tagged constants plus copyable config structs.
//
// All values are starting points unless tagged [HW] (F10 §8); tune with
// recorded data (F28/F29) and log the tuned values back into the spec's
// constants table. The config struct exists so tests (and later F18 settings)
// can override a threshold without touching code - no globals, config arrives
// by value (embedded conventions).

#include <cstddef>
#include <cstdint>

#include <bw/alerts.h>
#include <bw/state.h>

namespace bw::alert {

// --- ladder / policy thresholds (F10 §8) ------------------------------------

// [EXPERIMENTAL] Time in continuous Warning demand before Active (F10 §8,
// demo requirement 2026-10-18). Why 10 s: the demo escalates exactly 10 s
// after an unanswered warning; tune with recorded data (F29).
constexpr uint32_t kWarningToActiveMs = 10000;

// [USER] After AcknowledgeAlert, suppress a demand at or below the
// acknowledged level for 5 min (F10 §8). Why 5 min: long enough to finish the
// task the user was interrupted for, short enough that a still-drowsy user is
// warned again in the same session. Runtime-configurable through AlertConfig;
// the F18 SettingKey registration happens when F18 lands.
constexpr uint32_t kAckSnoozeMs = 300000;

// [EXPERIMENTAL] Quiet window after an alert resolved by itself or was
// cancelled (F10 §8). Why 10 min: "prevents nagging after a resolved alert" -
// long enough that the same condition does not re-alert in a loop, short
// enough that a real new episode is still caught.
constexpr uint32_t kCooldownMs = 600000;

// [EXPERIMENTAL] Hard maximum time spent in Persistent (F10 §6 D3): battery
// and annoyance bound. Why 2 min: long enough to wake someone, then the alert
// gives up and logs that it gave up.
constexpr uint32_t kPersistentMaxMs = 120000;

// [EXPERIMENTAL] Tone requested from a passive buzzer (F10 §8); typical
// resonance of small piezo discs sits in the low kHz. Active buzzers ignore
// freq_hz (bw/hardware.h, A-02), so this value cannot break that build.
constexpr uint16_t kBuzzerFreqHz = 2700;

// --- staleness and hysteresis (F10 §8 additions, see §6 D6) ------------------

// [EXPERIMENTAL] No new StateSnapshot for this long ⇒ the input is stale
// (F10 §10). Why 8 s: eight missed 1 Hz snapshots, with 2 s margin before the
// 10 s Warning escalation timer; tune against recorded pipeline timing (F29).
constexpr uint32_t kStateStaleMs = 8000;

// [EXPERIMENTAL] A lower demand must hold this long before the level drops
// (F10 §6 D1 hysteresis). Why 1 s: F07 already requires sustained/dwelling
// state recovery, so one 1 Hz state step stops an obsolete red alert promptly;
// still strictly below Warning escalation so Active-to-Warning is reachable.
constexpr uint32_t kDeescalateHoldMs = 1000;

// [EXPERIMENTAL] How long an event notification floors the demand above the
// state-derived level (F10 §6 D4). Why 30 s: the notification is instantaneous
// while the engine steps at 20 Hz; one snooze-length window is long enough for
// the user to notice, short enough that a single event cannot hold the buzzer.
constexpr uint32_t kEventAlertHoldMs = 30000;

// [DERIVED] AlertTask period (F10 §11: "AlertTask 20 Hz") = 1/20 s. Also the
// quantisation bound on pattern transitions: the player is ticked at this
// rate, so a step boundary can only land within one period of its ideal time.
constexpr uint32_t kAlertTickPeriodMs = 50;

// [EXPERIMENTAL] Filming protocol cadence, independent of movement or model priors.
constexpr uint32_t kDemoCheckIntervalMs = 20000;
// [EXPERIMENTAL] Time to answer the orange check before the red alert latches.
constexpr uint32_t kDemoResponseWindowMs = 5000;
// [EXPERIMENTAL] Strong-motion onset followed by eight seconds of stillness asks first.
constexpr uint32_t kDemoBurstStillnessMs = 8000;
// [DERIVED] One 1 Hz evidence window plus half a period of scheduling slack.
constexpr uint32_t kDemoEvidenceGapMs = 1500;
// [DERIVED] Original window-END -> F02 publish (1000 ms) -> F06 consume
// (1000 ms) -> F07 snapshot (1000 ms) -> F10 tick (50 ms), plus 500 ms
// scheduling slack = 3550 ms. This is AGE, not the stillness continuity gap.
constexpr uint32_t kDemoMotionFreshnessMs = 3550;
// [DERIVED] One diagnostic per 1 Hz evidence window; transitions also log immediately.
constexpr uint32_t kDemoTelemetryPeriodMs = 1000;

// --- Event.payload layout of an AlertRaised event (F10 §6 D8) ---------------

// Phase of the alert lifecycle this event reports (Event.payload[0]).
// 0 = Raised is also the "no phase" default, so a zeroed payload is never a
// plausible-looking value for an event that was actually built.
enum class AlertPhase : uint8_t {
    Raised = 0,                  // level entered (payload[1] = the new level)
    Resolved = 1,                // condition cleared on its own
    Acknowledged = 2,            // stopped by AcknowledgeAlert
    Cancelled = 3,               // stopped by CancelAlert
    AutoStoppedMaxDuration = 4,  // Persistent hit kPersistentMaxMs (§6 D3)
    StateStale = 5,              // stopped by kStateStaleMs (§10)
};

// [DERIVED] Field positions inside Event::payload (02 §4.5: 8 type-specific
// bytes; AlertRaised is F10's event type, so F10 defines their meaning here).
constexpr size_t kAlertPayloadPhaseIndex = 0;  // AlertPhase
constexpr size_t kAlertPayloadLevelIndex = 1;  // AlertLevel
// [DERIVED] payload[2..7] stay 0: reserved for a future field, and a zero
// tail lets a decoder stop at the last field it knows (02 §1 no-magic rule).
constexpr size_t kAlertPayloadReservedBytes = 6;

// [DERIVED] F10's feature id, written into Event.detector_id so a decoder can
// tell an alert record from a detector record without guessing (02 §4.5).
constexpr uint8_t kAlertFeatureId = 10;

// [HW] Event::duration_ms and AlertStatus::since_ms are uint32 fields
// (02 §4.5/§4.6): saturate instead of wrapping on a >49-day uptime.
constexpr uint32_t kMaxDurationFieldMs = 0xFFFFFFFFu;

// --- ReasonCode bit helper ---------------------------------------------------

// Bit for one ReasonCode (02 §4.5: ReasonMask is the bitset). F10-local on
// purpose: it depends on nothing but bw_core, and if a second feature needs it
// it belongs in bw_core - raise a change request instead of copying.
constexpr ReasonMask reasonBit(ReasonCode code) {
    return ReasonMask{1} << static_cast<uint8_t>(code);
}

// --- OperatingMode intensity profile (F10 §6 D7) ----------------------------

// [DERIVED] Number of OperatingMode enumerators (Day..Research, 02 §4.5).
// The enum is append-only (02 §6.6), so modeIntensity() clamps an unknown
// value instead of indexing off the end of the table.
constexpr size_t kModeIntensityCount = 5;

// [HW] Rgb channels are uint8_t and 255 is full-scale PWM duty (02 §3).
constexpr uint8_t kLedFullScale = 255;

// [DERIVED] Denominator for led_gain_pct (00 §7.5: the unit lives in the name).
constexpr uint16_t kPercentFull = 100;

// Per-mode output profile: how loud/bright an alert may be and how far the
// ladder may climb in that mode. Read from StateSnapshot::mode (F24 owns the
// mode itself; F10 only consumes the profile).
struct ModeIntensity {
    AlertLevel max_level;  // ceiling for the ladder in this mode
    uint8_t led_gain_pct;  // LED brightness, percent of full scale
    bool buzzer_enabled;   // false ⇒ patterns stay silent (LED/banner only)
};

// Defaults, one row per OperatingMode in enumerator order (02 §4.5).
// [ASSUMED] F10 §13: every mode keeps the full ladder and differs only in
// intensity - the spec's Inputs line says mode supplies an "intensity
// profile", and the night/persistent and planned-nap questions belong to
// F24/F15. The max_level field is their extension point: changing one row
// (e.g. Nap ⇒ Active) caps the ladder with no engine change.
//  - Day/Calibration/Research: full brightness, audible.
//  - Night/Nap: 60 % LED so a night-time banner does not blind; the buzzer
//    stays enabled because waking a drowsy user is the point of the device,
//    and the hardware has no volume control (02 §3: on/off + freq only).
inline constexpr ModeIntensity kDefaultModeIntensity[kModeIntensityCount] = {
    {AlertLevel::Persistent, 100, true},  // Day
    {AlertLevel::Persistent, 60, true},   // Night
    {AlertLevel::Persistent, 60, true},   // Nap
    {AlertLevel::Persistent, 100, true},  // Calibration
    {AlertLevel::Persistent, 100, true},  // Research
};

// Runtime copy of every threshold. Defaults are the named constants above;
// trivially copyable, so it is safe to hold by value in the policy and to pass
// through queues/mailboxes (02 §1).
struct AlertConfig {
    bool demo_wake_protocol = false;  // opt-in; standard ladder remains unchanged
    uint32_t demo_check_interval_ms = kDemoCheckIntervalMs;
    uint32_t demo_response_window_ms = kDemoResponseWindowMs;
    uint32_t demo_burst_stillness_ms = kDemoBurstStillnessMs;
    uint32_t warning_to_active_ms = kWarningToActiveMs;
    uint32_t ack_snooze_ms = kAckSnoozeMs;
    uint32_t cooldown_ms = kCooldownMs;
    uint32_t persistent_max_ms = kPersistentMaxMs;
    uint32_t state_stale_ms = kStateStaleMs;
    uint32_t deescalate_hold_ms = kDeescalateHoldMs;
    uint32_t event_alert_hold_ms = kEventAlertHoldMs;
    uint16_t buzzer_freq_hz = kBuzzerFreqHz;
    ModeIntensity mode_intensity[kModeIntensityCount] = {};

    constexpr AlertConfig() {
        for (size_t i = 0; i < kModeIntensityCount; ++i) {
            mode_intensity[i] = kDefaultModeIntensity[i];
        }
    }

    // Unknown or appended mode values fall back to Day: an append-only enum
    // (02 §6.6) must never index off the end of a fixed table.
    constexpr ModeIntensity modeIntensity(OperatingMode mode) const {
        const size_t index = static_cast<size_t>(static_cast<uint8_t>(mode));
        const size_t safe =
            (index < kModeIntensityCount) ? index : static_cast<size_t>(OperatingMode::Day);
        return mode_intensity[safe];
    }
};

}  // namespace bw::alert
