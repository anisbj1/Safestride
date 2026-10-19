#pragma once

// bw/ui/text_layout.h — F12 D7: truncating line formatter.
//
// IDisplayCanvas gives no font metrics, so long strings must not wrap and must
// never write outside their fixed line buffer. Every formatter copies THROUGH
// snprintf into a fixed std::array<char, N> line buffer: snprintf truncates
// safely at N-1 chars and always NUL-terminates, and any '%' inside caller
// data is passed as a %s argument so it can never be interpreted as a format
// directive (§9 test_text_layout). No wrapping in v1 (§12 out of scope).

#include <array>
#include <cstdio>

#include <bw/ui/ui_config.h>

namespace bw::ui {

// Font-sized line buffers (audit F-2): each render path truncates at ITS
// font's cap, so a future long Medium/Large string can never be drawn
// un-truncated per font — the buffer enforces the cap, not caller discipline.
using SmallLineBuffer = std::array<char, kMaxLineCharsSmall + 1>;
using MediumLineBuffer = std::array<char, kMaxLineCharsMedium + 1>;
using LargeLineBuffer = std::array<char, kMaxLineCharsLarge + 1>;
// Offset-aware buffers: the banner text starts right of the warning icon and
// the menu entries right of the cursor, so their caps are tighter (F-2).
using BannerLineBuffer = std::array<char, kMaxLineCharsBanner + 1>;
using MenuTextLineBuffer = std::array<char, kMaxLineCharsMenuText + 1>;

// Generic buffer at the widest cap; font-agnostic helpers and budget tests
// use it. Render paths use the font-sized aliases above.
using LineBuffer = SmallLineBuffer;

// Formats `fmt` (printf-style, caller-owned literal) into `out`. Truncation is
// NUL-safe; a negative snprintf return (encoding error) leaves an empty line,
// never garbage.
template <size_t N, class... Args>
void formatLine(std::array<char, N>& out, const char* fmt, Args... args) {
    static_assert(N >= 2, "a line buffer needs room for one char plus the NUL");
    const int written = std::snprintf(out.data(), N, fmt, args...);
    if (written < 0) {
        out[0] = '\0';
    }
}

// Copies plain text (never a format string — '%' stays literal) into `out`,
// truncated at N-1 chars, always NUL-terminated.
template <size_t N> void copyTruncated(std::array<char, N>& out, const char* text) {
    static_assert(N >= 2, "a line buffer needs room for one char plus the NUL");
    formatLine(out, "%s", text != nullptr ? text : "");
}

}  // namespace bw::ui
