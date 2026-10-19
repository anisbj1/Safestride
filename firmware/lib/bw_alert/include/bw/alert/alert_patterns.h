#pragma once

// bw/alert/alert_patterns.h - F10 §6 D2/D3: the pattern tables the player
// steps through, and the level ⇒ pattern mapping.
//
// A pattern is a list of fixed-duration steps (F10 D2: patterns are data, not
// code - no delay() anywhere, the player derives the current step from time).
// Tables are header-only constexpr so AlertPatterns is configuration, not
// behaviour, and a test can read the exact timings it must observe.
//
// Duty cycles are the only "volume" this hardware has (02 §3: buzzer is
// on/off + frequency, no amplitude): Active is a beep burst with a long rest,
// Persistent is near-continuous - louder by repetition, not by amplitude.

#include <cstdint>

#include <bw/alert/alert_config.h>
#include <bw/alerts.h>
#include <bw/hardware.h>

namespace bw::alert {

// Which table the player runs. Deliberately NOT AlertLevel: the policy talks
// levels, the player talks patterns, and the engine maps one to the other
// (so a future level change never has to reshuffle the player's API).
enum class PatternId : uint8_t { Off, Warning, Active, Persistent };

// One step: hold `led`/`buzz` for duration_ms, then move to the next step.
// The last step wraps to the first (patterns repeat while selected).
struct PatternStep {
    uint32_t duration_ms;
    Rgb led;
    bool buzz;
    uint16_t freq_hz;  // ignored by an active buzzer (02 §3)
};

struct Pattern {
    const PatternStep* steps;
    uint8_t count;
};

// --- colours ----------------------------------------------------------------

// [HW] Black = every channel at 0 % duty = LED off (02 §3).
inline constexpr Rgb kLedOff{0, 0, 0};

// [EXPERIMENTAL] Amber (red at full, green at half): reads as "caution" and
// stays visible in daylight; tune at the bench with the actual LED (F10 §6 D3).
inline constexpr Rgb kLedWarning{kLedFullScale, 128, 0};

// [EXPERIMENTAL] Red for Active and Persistent: the loudest colour on a
// two-colour-capable RGB part, reserved for "attend to this now".
inline constexpr Rgb kLedActive{kLedFullScale, 0, 0};

// --- step durations ---------------------------------------------------------

// [EXPERIMENTAL] Half-period of the Warning blink. Why 500 ms: a 1 Hz blink
// is the conventional "attention" rate - noticeable, not frantic.
constexpr uint32_t kBlinkHalfMs = 500;

// [EXPERIMENTAL] One short beep in the Warning onset cue. Why 150 ms: brief
// but audible; tune at the bench (F10 demo requirement).
constexpr uint32_t kWarningCueToneMs = 150;

// [EXPERIMENTAL] One Active beep. Why 500 ms: visibly longer than the 150 ms
// Warning cue, while retaining silent gaps between tones; tune at the bench.
constexpr uint32_t kActiveToneMs = 500;

// [EXPERIMENTAL] Gap between beeps inside a burst. Why 250 ms:
// longer than the tone, so the beeps are countable instead of a trill.
constexpr uint32_t kBeepGapMs = 250;

// [EXPERIMENTAL] Rest after three beeps. Why 3000 ms: Active must nag without
// becoming a continuous tone - ~11 % buzzer duty keeps it noticeable and
// leaves room for Persistent to be clearly worse.
constexpr uint32_t kBeepRestMs = 3000;

// [EXPERIMENTAL] Persistent tone / gap (F10 §6 D3: "louder/longer").
// Why 500/300 ms: ~63 % buzzer duty versus Active's ~11 % - the repetition
// rate is the only lever the hardware offers for "louder".
constexpr uint32_t kToneOnMs = 500;
constexpr uint32_t kToneGapMs = 300;

// [DERIVED] The Off pattern has no observable transitions, so any positive
// duration works; 1 s keeps the player's modulo walk cheap and bounded.
constexpr uint32_t kIdleStepMs = 1000;

// --- tables -----------------------------------------------------------------

inline constexpr PatternStep kOffSteps[] = {
    {kIdleStepMs, kLedOff, false, 0},
};

inline constexpr PatternStep kWarningSteps[] = {
    // Two short onset beeps; following amber blink tail keeps the complete
    // cycle longer than the 10 s escalation, so the cue cannot repeat first.
    {kWarningCueToneMs, kLedWarning, true, kBuzzerFreqHz},
    {kBeepGapMs, kLedWarning, false, 0},
    {kWarningCueToneMs, kLedWarning, true, kBuzzerFreqHz},
    {kBeepGapMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
    {kBlinkHalfMs, kLedWarning, false, 0},
    {kBlinkHalfMs, kLedOff, false, 0},
};

// Three beeps, then a rest - and it repeats while the level is Active.
inline constexpr PatternStep kActiveSteps[] = {
    {kActiveToneMs, kLedActive, true, kBuzzerFreqHz}, {kBeepGapMs, kLedOff, false, 0},
    {kActiveToneMs, kLedActive, true, kBuzzerFreqHz}, {kBeepGapMs, kLedOff, false, 0},
    {kActiveToneMs, kLedActive, true, kBuzzerFreqHz}, {kBeepRestMs, kLedOff, false, 0},
};

inline constexpr PatternStep kPersistentSteps[] = {
    {kToneOnMs, kLedActive, true, kBuzzerFreqHz},
    {kToneGapMs, kLedOff, false, 0},
};

template <size_t N> constexpr Pattern makePattern(const PatternStep (&steps)[N]) {
    return Pattern{steps, static_cast<uint8_t>(N)};
}

// Table lookup; an unknown id degrades to Off (silent, dark) rather than to
// some other pattern - a bad id must never become a louder alert.
constexpr Pattern patternFor(PatternId id) {
    switch (id) {
    case PatternId::Warning:
        return makePattern(kWarningSteps);
    case PatternId::Active:
        return makePattern(kActiveSteps);
    case PatternId::Persistent:
        return makePattern(kPersistentSteps);
    case PatternId::Off:
    default:
        return makePattern(kOffSteps);
    }
}

// F10 §6 D3 escalation ladder: level ⇒ pattern. Normal is silent/dark; each
// step above it is strictly more insistent (see duty cycles above).
constexpr PatternId patternForLevel(AlertLevel level) {
    switch (level) {
    case AlertLevel::Warning:
        return PatternId::Warning;
    case AlertLevel::Active:
        return PatternId::Active;
    case AlertLevel::Persistent:
        return PatternId::Persistent;
    case AlertLevel::Normal:
    default:
        return PatternId::Off;
    }
}

}  // namespace bw::alert
