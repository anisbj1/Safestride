#pragma once

// bw/ui/ui_config.h — F12 §8: every UI constant in one header, each named,
// tagged and justified (00 §7.5 "no magic"). UiConfig carries the per-instance
// knobs so host tests can shorten timeouts without touching production values.

#include <cstddef>
#include <cstdint>

#include <bw/types.h>

namespace bw::ui {

// --- display geometry --------------------------------------------------------

// [HW] SH1106 128x64 1-bit panel (02 §3 IDisplayCanvas).
constexpr uint8_t kScreenWidthPx = 128;
constexpr uint8_t kScreenHeightPx = 64;

// [ASSUMED] Glyph advance per font (§13: F01 renderer facts, verify at
// bench). Named so the char caps below and their static_asserts are
// arithmetic instead of magic numbers (audit F-2).
constexpr int16_t kGlyphWidthSmallPx = 6;
constexpr int16_t kGlyphWidthMediumPx = 8;
constexpr int16_t kGlyphWidthLargePx = 10;

// [DERIVED] Conservative per-font character caps (D7): 128 px divided by an
// assumed ~6 / ~8 / 10 px glyph advance; intentionally conservative so a line
// can never write past 128 px even if F01's font is a glyph wider than assumed.
// Glyph widths are F01 renderer facts `[ASSUMED]` (§13): verify at bench,
// adjust these constants only.
constexpr size_t kMaxLineCharsSmall = 21;
constexpr size_t kMaxLineCharsMedium = 16;
constexpr size_t kMaxLineCharsLarge = 12;

// [DERIVED] Alert-banner text origin (D3 overlay): 1 px margin + the 14 px
// warning icon (kWarningIconW in icons.h) + 3 px padding. The banner text
// starts here, NOT at x=0, so it needs its own tighter cap (audit F-2).
constexpr int16_t kBannerTextXPx = 18;
constexpr size_t kMaxLineCharsBanner =
    static_cast<size_t>((kScreenWidthPx - kBannerTextXPx) / kGlyphWidthMediumPx);

// [DERIVED] Menu entry text origin: 10 px leaves room for the ">" cursor at
// x=0 (renderMenu); entries are Small font, so their cap accounts for the
// offset the same way the banner cap does (audit F-2).
constexpr int16_t kMenuTextXPx = 10;
constexpr size_t kMaxLineCharsMenuText =
    static_cast<size_t>((kScreenWidthPx - kMenuTextXPx) / kGlyphWidthSmallPx);

// Pins (audit F-2): a line at its cap, drawn at its origin, must fit the
// 128 px panel under the assumed glyph widths — for the full-width caps and
// for the two offset texts alike. A future offset or width tweak that would
// overflow fails the build, not the bench.
static_assert(kMaxLineCharsSmall * kGlyphWidthSmallPx <= kScreenWidthPx,
              "a full-width Small line must fit the panel");
static_assert(kMaxLineCharsMedium * kGlyphWidthMediumPx <= kScreenWidthPx,
              "a full-width Medium line must fit the panel");
static_assert(kMaxLineCharsLarge * kGlyphWidthLargePx <= kScreenWidthPx,
              "a full-width Large line must fit the panel");
static_assert(kBannerTextXPx + kMaxLineCharsBanner * kGlyphWidthMediumPx <= kScreenWidthPx,
              "a full-cap banner line after the icon must fit the panel");
static_assert(kMenuTextXPx + kMaxLineCharsMenuText * kGlyphWidthSmallPx <= kScreenWidthPx,
              "a full-cap menu entry after the cursor must fit the panel");

// [EXPERIMENTAL] Line pitch per font (glyph height + ~2 px gap), used for the
// fixed y positions of the screen layouts; no wrapping in v1 (§12), so the
// pitch only decides row spacing.
constexpr int16_t kLinePitchSmall = 10;
constexpr int16_t kLinePitchMedium = 16;
constexpr int16_t kLinePitchLarge = 20;

// [DERIVED] Alert-banner band height (D3 overlay): one Medium line plus 2 px
// padding on each side; the band is filled and drawn OVER the active screen.
constexpr uint8_t kBannerHeightPx = 16;

// --- timing (spec §8) --------------------------------------------------------

// [EXPERIMENTAL] Frame interval == task period: 10 fps is plenty for status
// text and keeps I²C bus load low (spec §5 redrawing cap).
constexpr bw::TimeMs kUiFrameIntervalMs = 100;

// [USER] Screen off after this much inactivity (power); alerts wake it (D8).
// [USER]-intended: until F18 registers a SettingKey it is a compile-time
// default (§8). 0 = DISABLED, screen never sleeps (testing/presentation
// build, developer 2026-10-18). History: 15 s at bring-up, 30 s (2026-10-15)
// because F02 HR needs ~10-30 s of steady contact before quality reaches
// Good, 60 s (2026-10-18), then disabled for the demo/test sweep. Restore a
// positive value (e.g. 60000) to re-arm D8. Usability knob only - no signal
// change.
constexpr bw::TimeMs kDisplayTimeoutMs = 0;

// [EXPERIMENTAL] Minimum banner show time (D3): prevents flicker while an
// alert level oscillates around Warning.
constexpr bw::TimeMs kAlertScreenMinShowMs = 3000;
// [EXPERIMENTAL] Half-second on/off gives a readable 1 Hz demo prompt.
constexpr bw::TimeMs kDemoBannerBlinkHalfPeriodMs = 500;
// [HW] F01's Large font is u8g2 10x20, fixed 10-pixel advance / 20-pixel box.
constexpr int16_t kDemoBannerGlyphHeightPx = 20;

// [EXPERIMENTAL] PpgFeatures arrive at 1 Hz (§4.2); 10 s without a new valid
// frame means the HR readout shows "--" rather than a stale number (§8).
constexpr bw::TimeMs kHrStaleMs = 10000;

// [EXPERIMENTAL] Consecutive failed flushes (~2 s at 10 fps) before ONE
// IFaultReporter report (§10): transient I²C hiccups retry silently, a
// persistent failure is reported once per episode, never per frame.
constexpr uint8_t kFlushFailFaultThreshold = 20;

// --- per-instance knobs ------------------------------------------------------

// UiConfig — the runtime knobs of one UiModel / ScreenRenderer instance.
// Defaults are the spec §8 values above; host tests shorten them to keep
// FakeClock timelines small. A knob exists only if a test needs to vary it.
struct UiConfig {
    bw::TimeMs display_timeout_ms;    // kDisplayTimeoutMs
    bw::TimeMs alert_min_show_ms;     // kAlertScreenMinShowMs
    bw::TimeMs hr_stale_ms;           // kHrStaleMs
    uint8_t flush_fail_threshold;     // kFlushFailFaultThreshold
    bool demo_wake_protocol = false;  // opt-in full-screen prompts; standard overlay unchanged
};

constexpr UiConfig defaultUiConfig() {
    return UiConfig{kDisplayTimeoutMs, kAlertScreenMinShowMs, kHrStaleMs, kFlushFailFaultThreshold};
}

}  // namespace bw::ui
