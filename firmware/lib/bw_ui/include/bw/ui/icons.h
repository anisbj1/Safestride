#pragma once

// bw/ui/icons.h — F12 §7: tiny XBM bitmaps for the status bar and banner.
//
// XBM layout (02 §3 drawBitmap): each row is (width + 7) / 8 bytes, LSB =
// leftmost pixel. Pixel content is [ASSUMED] art (§13): recognizability is
// verified at bench (HIL), these sizes and bytes are F12-owned constants.

#include <cstdint>

#include <bw/ui/ui_config.h>

namespace bw::ui {

// [DERIVED] Battery outline for the Home screen's bottom row: 12x8 px fits
// the Medium line pitch (kLinePitchMedium) with 4 px of text next to it.
constexpr uint8_t kBatteryIconW = 12;
constexpr uint8_t kBatteryIconH = 8;
inline constexpr uint8_t kBatteryIconXbm[kBatteryIconH * 2] = {
    0xFF, 0x0F,  // ┌────────┐  top edge (12 px)
    0x01, 0x08,  // │        │  sides only
    0x01, 0x08, 0x01, 0x08, 0x01, 0x08,
    0x01, 0x08, 0x01, 0x08, 0xFF, 0x0F,  // └────────┘  bottom edge
};

// [DERIVED] Warning triangle for the alert banner (D3): 14x12 px sits inside
// the kBannerHeightPx band with a 1 px margin, leaving the rest of the band
// for the level text.
constexpr uint8_t kWarningIconW = 14;
constexpr uint8_t kWarningIconH = 12;
inline constexpr uint8_t kWarningIconXbm[kWarningIconH * 2] = {
    0x40, 0x00,              //      ▲        apex
    0xE0, 0x01,              //     ███
    0xF0, 0x03,              //    █████
    0xF0, 0x03, 0xF8, 0x07,  //    ███████
    0xF8, 0x07, 0xFC, 0x0F,  //   █████████
    0xFC, 0x0F, 0xFE, 0x1F,  //  ███████████
    0xFE, 0x1F, 0xFF, 0x3F,  // ██████████████  base
    0xFF, 0x3F,
};

}  // namespace bw::ui
